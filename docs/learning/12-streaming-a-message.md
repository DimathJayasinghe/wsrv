# Lesson 12 — Streaming a message

## Goal
Send a message you cannot (or do not want to) hold in memory: a file, a database export, a live feed of unknown length. After this lesson you can explain how `MessageWriter` keeps memory bounded, why it needs a callback (`on_writable`), why a dropped writer closes the connection, and what an edge-triggered `epoll` registration has to do with all of it.

## 1. The problem
`send_binary(bytes)` needs the whole message first, and the outbound queue must hold all of it (`max_queued_bytes`). Sending 8 MiB through a 64 KiB queue is simply refused (`BufferFull`). RFC 6455 section 5.4 already has the answer on the wire: a message may be **fragmented**, so the server can send the first piece before the last one exists. The work is to expose that safely: a handler writes pieces, and each piece is sent when the peer can take it.

```text
handler:   write(16K) write(16K) write(16K) write(16K)=Backpressure ... on_writable: write(16K) ...
queue:     [ at most max_queued_bytes, whatever the message size ]
wire:      Binary(FIN=0) Cont(FIN=0) Cont(FIN=0) ............................ Cont(FIN=1, empty)
```

## 2. Three layers, one idea each
| Layer | Addition | Idea |
|---|---|---|
| `OutboundScheduler` | `begin_stream / append_stream / end_stream` | An **open** message sits at the end of the data queue and grows. A frame is made from whatever bytes exist; the message ends with FIN. If everything was already sent when `end_stream` arrives, an **empty final Continuation** carries the FIN (valid, EC-F37). Control frames still win at every frame boundary (FR-F10). |
| `ProtocolSession` | `begin_message / write_part / end_message` | Owns the rules: text is checked as UTF-8 **across** parts (a character may be split between two writes), and a message may not end inside a character. While a stream is open no other data message may start (`MessageInProgress`, EC-F33). Closing the session discards it. |
| `Connection` / `MessageWriter` | `Session::begin_text()`, `on_writable` | The public, application-facing half. Lifetime and flow control live here. |

Why split it this way? The scheduler and session stay free of sockets, so they can be tested byte by byte and fuzzed (`fuzz_session` now has begin/write/end operations and checks the fragmentation grammar of everything it emits).

## 3. A refused part leaves no trace
`write_part` validates UTF-8 on a **copy** of the validator and only keeps the new state if the queue also accepted the bytes. Without the copy, a part that passes UTF-8 but is then refused by the queue (`Backpressure`) would leave the validator "ahead" of the data actually queued, and the retry would be judged against the wrong state. Rule: *a failed call changes nothing* (same rule as `send_text`).

## 4. The cooperative protocol and `on_writable`
`write()` returns `Backpressure` when the piece does not fit. That is not an error to log; it means "stop, return from the callback, I will call you". The connection remembers that it owes a notification. When the queue has drained to half the limit (the **low-water mark**; "does not fit" is the high-water mark) it calls `on_writable`, once, on a **later loop iteration** than the refusal. The later iteration matters: a handler streaming a gigabyte must not run in a tight loop inside one event, or it starves every other connection (EC-N08, same reason reads have a budget).

How does a connection get "a later iteration" without a loop of its own? It arms **write interest** (`EPOLLOUT`) while a notification is owed. A socket is nearly always writable, so the kernel reports it at the next `epoll_wait`, the connection delivers `on_writable` and disarms.

### The bug this found (read this twice)
The first version armed write interest when the notification became owed, delivered it, and then, if the handler was refused *again*, simply left interest armed. The stream stalled after about 100 KiB. Why: the registration is **edge-triggered** (lesson 10). The kernel reports a state *change*, and re-registering with the *same* flags changes nothing, so no new event came. The fix: disarm before calling `on_writable`; if the handler is refused again, the next `settle()` arms it again, and *that* arming is what makes `epoll` report the writable socket once more. The unit tests with a fake host passed (the fake reports whatever you tell it); the real-socket test `ServerStream.AMessageFarLargerThanTheQueueIsStreamedInBoundedMemory` found it. Lesson: model-based tests cannot find a bug in the model's assumption; keep at least one test against the real kernel.

## 5. A writer can outlive its connection
A handler may keep the writer between callbacks, but the connection can end at any time (ADR-0013). So the writer never holds a pointer to the connection. It holds a `shared_ptr<StreamLink>`, and the link holds the one raw pointer, which the connection **clears** when it finishes or is destroyed. A late call finds `nullptr` and returns `Closing`. Everything is on the loop thread, so the link needs no atomics. (Compare `SessionHandle` in lesson 11, which crosses threads and therefore needs a mutex.)

## 6. Why a dropped writer closes the connection (1011)
If the handler destroys the writer without `finish()`, the peer has received fragments of a message that never ends. We cannot take bytes back. Finishing it ourselves would publish a truncated message as if it were complete, which is worse. So the connection is closed with 1011 and logged (EC-F34). The same applies when a handler throws mid-stream: the writer is destroyed during unwinding and the connection is closed exactly once. If the message is already gone because the *peer* closed first, nothing is blamed on the handler.

## 7. No slow-consumer policy for writers
`send_*` applies the policy (close with 1008, or drop the message) because it cannot wait. A writer can wait, so `Backpressure` on a write is only a signal. A peer that never reads is still removed by the write-stall timer (T3); that is a time bound, independent of how cooperative the handler is.

## 8. Tests, and what each proves
- `OutboundStream.*` (scheduler) and `SessionStream.*` (session): framing, empty final frame, UTF-8 split across parts, refused parts, close discards the stream.
- `ConnectionTest` streaming section (18 tests, fake transport): fragments, resume from `on_writable`, once-only delivery, not in the same event, the drop and throw cases, writer outliving the connection and the reverse, moved-from writers.
- `ServerStream.*` (real sockets): an 8 MiB message through a 64 KiB queue arrives intact (and `send_binary` of the same size is refused, so the contrast is in the test); text with characters cut between writes; a Pong answered in the middle of a stream; a client vanishing mid-stream.

## 9. Pitfalls
- **Ignoring `Backpressure`** and calling `write` again in a loop: every call fails, nothing is queued, CPU burns. Return and wait.
- **Waiting for `on_writable` that was never owed:** it only follows a refusal. Start the stream with a write, and resume from the callback.
- **Calling `finish()` from a different thread:** a writer is loop-thread-only. Cross-thread sends use `SessionHandle`, which is whole-message.
- **Storing the writer in a handler that outlives `on_close` expectations:** fine, but every call returns `Closing` afterwards.

## 10. Exercises
1. Remove the `apply_interest()` call before `on_writable` and run `ServerStream.*`. Which test fails and how far does the stream get? Why do the `ConnectionTest` ones still pass?
2. Why is the low-water mark half the limit and not zero? *(A stream that waits for an empty queue idles the link between refills; half keeps data flowing while the handler refills.)*
3. A text stream writes `E2 82` and calls `finish()`. What comes back and what state is the writer in? Then write `AC` and finish again.
4. Make the handler throw halfway through a stream. How many Close frames does the peer see?
5. Why does `write_part` validate on a copy instead of validating first and rolling back?
