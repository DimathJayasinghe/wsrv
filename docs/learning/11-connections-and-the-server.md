# Lesson 11 — Connections and the server

## Goal
Join the pieces of lessons 05–10 into a working server: bytes arrive on a socket, become a handshake, then WebSocket messages, reach your handler, and the replies go back, while slow, silent, flooding and buggy peers are contained. After this lesson you can read `src/server/` top to bottom and say what each class owns and why nothing destroys itself.

## 1. The problem in one picture
Until M5 every piece was tested alone: the protocol core knows no sockets, the net module knows no WebSocket. The server layer is the glue, and glue is where lifetime and ordering bugs live. One connection is a small state machine driven by three kinds of event:

```text
   readable / writable (epoll)          timers (wheel)              handler calls
            \                                |                           /
             +----------------------->  Connection  <-------------------+
                                             |
        Handshaking --101--> Established --Close--> Finished --retire--> erased by the table
```

## 2. The cast
| Class | One-line job | Why it exists |
|---|---|---|
| `HandshakeDriver` (protocol/) | Bytes in, `NeedMore / Accepted / Rejected` out | The handshake without I/O: testable one byte at a time (EC-H17) and reusable by M8 (TLS) and M11 (C ABI). |
| `Connection` | One client: transport + driver + `ProtocolSession` + handler + timers | The only class that knows all the parts. |
| `IConnectionHost` | What a connection may ask of the world: timers, watching its fd, "I am finished" | Interface Segregation: a connection cannot reach the loop, the table or other connections. Tests give it a `FakeHost` with manual timers. |
| `LoopHost` | The real host, an adapter over `EventLoop` | Keeps `Connection` free of `net/` internals. |
| `ConnectionTable` | Owns connections by id; erases a retired one **later** | Section 5. |
| `Server` (Facade) | Loop, listener, acceptor, table, signals, shutdown | One object to start and stop. |
| `ServerBuilder` | Validates once, builds, the only place that throws | Everything after `build()` returns `Result`; a bad port is an operator error and says so (`ConfigError`). |
| `IWebSocketHandler`, `Session` | What the application writes and calls | The public contract (ADR-0013). |

## 3. Reading: budgets and the edge
epoll here is edge-triggered (lesson 10): readiness is reported once. So `on_readable` reads until `WouldBlock`... except that a flooding peer would make that last forever. The rule is a **budget** (`ReadBudget`: 16 reads or 64 KiB per event). When the budget runs out with data still pending, the connection calls `rewatch`, which re-arms the edge so the kernel reports again next iteration, after the other connections had their turn. The integration test `AFloodingClientDoesNotStarveAQuietNeighbour` is the proof: 300 × 16 KB frames from one client, and a quiet client's round trip stays under 2 s.

All connections of a loop share **one 16 KiB scratch buffer** to read into (`ConnectionContext`). That is safe because the loop runs one connection at a time and the bytes are fully consumed (or copied into the session's bounded buffers) before the next read. Memory per idle connection stays small.

## 4. Writing: only while there is something to write
`flush` tries to write at once. Most of the time the kernel accepts everything and nothing more is needed. If it accepts only part (a **partial write**, normal for TCP), the rest stays in the session's bounded output queue and the connection asks the loop for *write interest* (`EPOLLOUT`) until the queue is empty. Watching for writability permanently would wake the loop on every iteration for nothing: a socket is almost always writable.

Three layers of defence against a peer that does not read (T3, EC-N09):
1. **The bounded queue** (`max_queued_bytes`). Over it, `send_*` returns `Backpressure`.
2. **The slow-consumer policy**: `Close` (default, close 1008) or `DropMessage` (refuse the message, keep the connection).
3. **The write-stall timer**: bytes are queued and the kernel accepted none for `write_stall_timeout`, so the connection is dropped. The queue limit alone is not enough: a peer that reads 1 byte a minute never fills it but occupies a slot forever.

## 5. Why a connection never destroys itself
Suppose `on_readable` decides the connection is finished and runs `delete this`. The call stack still contains `on_readable`, and probably the handler callback that asked to close, and the event loop's own dispatch that holds a pointer to this connection's watch. Returning into freed memory is undefined behaviour, and sanitizers find it only if a test happens to take that path.

The rule (ADR-0013): **a connection ends by calling `retire()`; the `ConnectionTable` erases it from `EventLoop::defer`**. `defer` is a task that runs at a *safe point*: after the current batch of events, timers and tasks has finished, when no frame of this connection is on the stack. Same idea as a garbage collector's safe point, built from one queue.

Consequences worth knowing:
- `on_close` runs **exactly once**, and only if `on_open` ran (a handshake that never finished creates no handler: EC-H21).
- Inside `on_close` you may not send (EC-A03); inside any callback `close()` only records the request and the work happens after the callback returns (EC-A06).
- A handler that throws is caught at one place (`Connection::guarded`, the only `catch (...)` in the module, which logs and does not swallow): the connection closes with 1011 and the other thousand clients never notice (EC-A01, T16).

## 6. A bug found by UBSan: destruction order
`Server` owns the loop, host, scratch buffer, context, table, acceptor and signal watcher as members. C++ destroys members in *reverse declaration order*. The first version declared the table before the host. At shutdown the host died first; then the table destroyed each connection, and a connection's destructor called the host (to cancel its timers). UBSan reported a call on an object whose lifetime had ended.

The rule is now a comment in `server.hpp` and a test (`ChurnLeavesNoDescriptorsAndTheCapFreeAgain` plus the whole suite under ASan/UBSan): **everything a connection touches while being destroyed is declared before the table**; the acceptor and signal watcher come last so they are destroyed first and stop feeding new work. Lesson: destruction order is part of the design, and a comment is not a test, so we have both.

## 7. Shutdown and signals
`request_shutdown()` (any thread): stop accepting, send Close 1001 to every client, wait up to `shutdown_grace`, then drop whatever is left. A second request means "now" (EC-C18). Signals are **opt-in** (`install_signal_handlers()`): a library must not take over a process's signals unasked (EC-N17). It uses `signalfd` (a signal read as a file descriptor, so the handler is ordinary loop code, not an async-signal-safe minefield) and blocks only SIGINT and SIGTERM for the calling thread, so call it before starting other threads.

## 8. Tests, and what each layer proves
- `tests/server/connection_test.cpp` (26): one connection against `FakeTransport` and `FakeHost`: byte-at-a-time handshake, partial 101, budgets, every timer, every close path, throwing handlers. Fast and deterministic: timers are advanced by hand.
- `tests/server/server_test.cpp` (10): a real server on a loopback port: echo, refused Origin, connection cap, 100-connection churn (descriptor count returns to baseline), graceful and forced shutdown, signal, builder errors.
- `tests/integration/server_integration_test.cpp` (10): hostile clients: lengths at every encoding boundary, one byte over the limit (1009), slowloris, TLS hello on the plain port, slow reader, flooder, heartbeat.
- Autobahn (`tests/autobahn/`): an independent implementation attacking the whole thing. See `EXCLUDED.md`.

Bugs these tests found (each is a lesson):
1. UBSan: destruction order (section 6).
2. TSan: our own test closed a socket while another thread was still writing to it. Test code needs the same discipline as product code; the fix joins the thread first.
3. The demo: a maximum-size message was accepted but its **echo** was refused, because the outbound queue limit equalled the inbound message limit and the queue also charges 16 bytes per message. A limit on one side of a relay must be sized with the other side in mind.

## 9. Pitfalls
- **Destroying from inside a callback.** Retire, never delete (section 5).
- **Armed write interest with an empty queue.** Busy loop at 100% CPU.
- **Reading until `WouldBlock` without a budget.** One client monopolises the loop.
- **Holding the handler's `string_view` after the callback.** It points into the shared scratch buffer or the session's buffer (EC-A04).
- **Member order.** Section 6.
- **Calling the language runtime from the loop thread.** For bindings only (`docs/BINDINGS.md`); M7 adds `SessionHandle` for cross-thread sends.

## 10. Exercises
1. A client sends the handshake in a packet that also contains the first frame. Where do those frame bytes go? *(`HandshakeOutcome::consumed` stops at the head; `Connection` feeds the rest to the session: `AMessageInTheSameReadAsTheHeadIsDelivered`.)*
2. Why does the write-stall timer not replace `max_queued_bytes`? And the reverse? *(Section 4: the queue bounds memory, the timer bounds time.)*
3. Move `table_` to the end of the member list in `Server` and run the server tests under `ctest --preset dev`. What does the sanitizer say? Why does it not always fail?
4. Set `slow_consumer = DropMessage` and send 100 MiB at a client that never reads. What does the handler see, and what ends the connection? *(`send_*` returns `Backpressure`; the stall timer.)*
5. Why must `install_signal_handlers()` run before other threads start? *(`pthread_sigmask` affects only the calling thread; a thread created later inherits the mask, an earlier one does not.)*
