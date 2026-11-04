# Lesson 23 — Pumping TLS through memory (the `TlsTransport` decorator)

## Goal
After this lesson you can explain what a *decorator* is and why TLS is one here, how OpenSSL is driven without ever touching a socket, why an edge-triggered event loop needs two extra hints from a TLS transport, and what each test of the transport proves.

## 1. The decorator
`Connection` reads and writes through `ITransport` (lesson 10). `PlainTransport` implements it with `read(2)`/`send(2)` on a socket. `TlsTransport` implements the **same interface** and wraps another `ITransport`: bytes the connection writes are encrypted and handed to the inner transport; bytes read from the inner transport are decrypted before the connection sees them. The protocol layers, the handshake hook and the PHP binding never learn TLS exists (requirement FR-T2). That is the whole value of the pattern: add behaviour around an object without changing the object's users.

## 2. OpenSSL without a socket
OpenSSL normally reads and writes a file descriptor itself. We give it **two memory buffers** instead (a "memory BIO" is a byte queue with OpenSSL's read/write interface):

```
socket ──read──► [fill] ──► from_peer buffer ──► SSL_read / SSL_do_handshake
socket ◄─write── [flush] ◄── to_peer buffer   ◄── SSL_write / handshake output
```
- When OpenSSL says *want read* ("I need more ciphertext"), we `fill()`: one read from the inner transport into the first buffer, then retry.
- When it has produced output, we `flush()`: write it to the inner transport; whatever the inner transport refuses (`WouldBlock`) stays in the buffer.
- The handshake is not a step of its own. `read()` and `write()` run it first, and return `WouldBlock` until it completes, because `Connection` already knows how to wait on `WouldBlock`.

Why this and not "hand OpenSSL the descriptor"? The transport never blocks (it only ever calls functions that return at once), a test can feed any bytes (one at a time, cut in half, garbage), and the fuzzer can do the same.

## 3. Two things epoll cannot know
`epoll` is **edge-triggered** here: it tells us when something *changes*, once (lesson 10). A plain socket is fine with that. TLS breaks it in two places:

1. **Output we could not send.** A handshake step needs us to *write* (the server's certificate flight) while `Connection` only listens for *readable*. If the socket buffer is full at that moment, nobody will ever tell us it has room again, and the handshake hangs. Fix: `wants_write()`. While it is true the connection asks for "writable"; the writable event makes it call the transport again (`write({})` means "push out what you hold"). The existing write-stall timer bounds a peer that never reads.
2. **Input we already took.** OpenSSL gets up to 16 KiB of ciphertext per `fill`. If that holds three records and the connection stops after the first (read budget, back-pressure pause, waiting for the application to answer the hook), the other two are in OpenSSL's memory, not in the kernel: epoll will never mention them. Fix: `buffered_input()`. When it is true and the connection resumes, it schedules a read for itself (a zero-delay timer, so other connections get their turn).

Both are `virtual … { return false; }` on `ITransport`: plain transports and test fakes are unchanged.

## 4. Bounds
Every buffer has a limit and an outcome. Ciphertext waiting for the socket: 64 KiB, beyond that `write` says `WouldBlock`. One `read()` call takes at most 64 chunks from the socket, then yields (and says `buffered_input()`), so one hostile client cannot hold the loop. Plaintext per `write`: one TLS record (16 KiB).

## 5. What the tests prove
| Test | Proves |
|---|---|
| handshake one byte at a time, TLS 1.2 and 1.3 | EC-T01: any split of the bytes works |
| a socket that takes 0 / 1 / 700 bytes per call | the flush/retry logic; the 200 KB write arrives intact (EC-T06) |
| `wants_write` while the socket is full, cleared after | edge-triggered wake-up works |
| plain HTTP / garbage on a TLS port | EC-T02: `TlsFailure`, and it stays failed |
| `close_notify` vs cut connection | EC-T04: clean EOF (0) vs `ConnectionClosed` (becomes 1006) |
| corrupted record | integrity: `TlsFailure`, not wrong data |
| renegotiation attempt | EC-T05: no second handshake |
| bounded output | the 64 KiB cap |
| `buffered_input` read-back | records already taken are not lost |
| fuzzing (`fuzz_tls_server`) | hostile bytes never crash it, never produce data, never get an amplified reply |

The `Connection` tests use a fake transport with a "hold output" and a "hold input" switch, so the two hooks are tested without any TLS: output held → Write interest and a stall timeout; input held → read again after the budget, after a resume; and a plain transport schedules nothing.

## 6. Pitfalls we hit
- **The renegotiation test could not fail.** OpenSSL 3 refuses client renegotiation by default, so removing our option changed nothing the behavioural test could see. The option bit is pinned in the context test instead (lesson 22) and the transport test checks the end result. When a defence is redundant, test the *setting*, not just the outcome.
- **A client counter that lies.** `SSL_total_renegotiations` goes up when the client merely *starts* one. The reliable observation is what the *server* wrote back: a 31-byte alert, not a certificate flight.
- **A mutant that did not compile left an old binary running** and looked like a "survivor". Always check the build's exit code before reading test results.

## 7. Exercises
1. Why does `read()` return `ConnectionClosed` (not 0) when the TCP connection ends without `close_notify`? What would an attacker gain if it returned 0 (RFC 8446 section 6.1)?
2. `write({})` returns 0, and the connection treats "0 bytes accepted" as blocked. Walk through what happens during a handshake when the socket is full, then empties.
3. Raise `kMaxPendingOutput` to 4 GiB. Which test fails, and what attack does the cap stop?
4. Why is the reread a *timer* and not a loop inside `on_readable`?
