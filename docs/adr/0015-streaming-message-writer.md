# ADR-0015: Streaming message writer (`MessageWriter`) and `on_writable`

- Status: Proposed
- Date: 2026-10-30
- Deciders: project owner (to accept)

## Context
FR-F12 requires a non-blocking streaming writer: send a message of unknown or very large size piece by piece without holding it in memory, with backpressure. Today a handler can only send complete messages (`send_text`, `send_binary`); the scheduler frames them when it writes. The design intent is in FRAGMENTATION.md section 2.2 and EC-F33..F35; this ADR fixes how it is built, because it touches three layers (scheduler, session, connection) and a public type whose lifetime is subtle: a writer must be able to outlive the callback that created it (to continue after backpressure), yet a connection can die at any time (ADR-0013 section 3). Threats: T3 (slow reader), T16 (buggy handler). Requirements: FR-F9, FR-F10, FR-F12, FR-S4.

## Decisions

### 1. The scheduler learns about an *open* message
`OutboundScheduler` gains three operations: `begin_stream(opcode)`, `append_stream(bytes)`, `end_stream()`. An open message is the last entry of the data queue and is flagged `open`; bytes appended to it are charged against `max_queued_bytes` exactly like a whole message (`Backpressure` when they do not fit, nothing queued). While a message is open, `enqueue_message` and `begin_stream` fail with the new `ErrorCode::MessageInProgress` (EC-F33): RFC 6455 section 5.4 forbids interleaving two data messages.

Framing: the front message may be framed while it is still open. Whatever bytes are available are sent as a non-final frame (a Continuation after the first); when the message is ended and nothing remains, an **empty final Continuation frame** completes it (valid, EC-F37). An open message with no bytes available produces no frame and does not count as pending output. Control frames keep priority at every frame boundary (FR-F10), so a Pong or Close is never stuck behind a stream. `queue_close` discards an open message like any other unsent data and clears the open flag; later writer calls return `Closing` (EC-C14).

### 2. The session validates text and owns the rules
`ProtocolSession` exposes `begin_message(type)`, `write_part(bytes)`, `end_message()` and `message_open()`. For text it keeps one incremental UTF-8 validator for the open message: `write_part` validates on a trial copy and queues nothing on `InvalidUtf8` (a fragment may split a code point, RFC 6455 section 5.6, so only the cumulative state matters); `end_message` fails with `InvalidUtf8` if the message ends inside a code point and leaves the message open. The scheduler stays ignorant of UTF-8.

### 3. `MessageWriter` is loop-thread-only and cannot dangle
`Session::begin_text()` / `begin_binary()` return `Result<MessageWriter>`. The writer is a small move-only object in `src/server/` that holds a `std::shared_ptr<StreamLink>`; `StreamLink` holds one pointer to the connection's stream interface and is **cleared by the connection when it finishes or is destroyed** (loop thread only, so no atomics are needed). A writer used after its connection ended gets `Error{Closing}` and does nothing; destroying such a writer is a no-op. A writer must be used on its connection's loop thread; cross-thread streaming is out of scope (`SessionHandle` stays whole-message).

`write()` returns `Backpressure` when the bytes do not fit (nothing is queued); `finish()` returns `InvalidUtf8` for an incomplete text message. **A writer destroyed while its message is open closes the connection with 1011 and logs** (EC-F34): fragments already on the wire cannot be taken back, and a silently truncated message would be worse than a closed connection. The destructor calls the connection; it never throws.

### 4. `on_writable` and watermarks
`IWebSocketHandler::on_writable(Session&)` is a new optional callback. When a write (or a `send_*` under the `DropMessage` policy) returned `Backpressure` to the application, the connection remembers it. It calls `on_writable` **once** when the queue has drained to half of `max_queued_bytes` (the low-water mark; the high-water mark is "does not fit"), then forgets. The wake-up is driven by write readiness, not by a loop of its own: while a notification is owed the connection keeps write interest armed, so the next loop iteration delivers it. One notification costs one iteration, which keeps a streaming handler from monopolising the loop (EC-N08). The callback runs inside the usual exception barrier.

### 5. No slow-consumer policy for writers
`send_*` applies the slow-consumer policy (close 1008 by default, ADR-0013 section 4) because it cannot wait. A writer is cooperative: `Backpressure` is the signal to stop and wait for `on_writable`, so the policy does **not** fire on it. A handler that ignores the signal gains nothing (its writes fail and nothing is queued); a peer that never reads is still dropped by the write-stall timer (T3).

## Consequences
+ A large message streams in bounded memory: at most `max_queued_bytes` is ever queued, whatever the message size.
+ The scheduler and session stay sans-I/O and fuzzable; `fuzz_session` should gain `begin/write/end` operations.
+ A writer can never touch a freed connection; the price is one `shared_ptr` per open writer.
- A handler must follow the protocol (write, wait for `on_writable`, finish); a handler that abandons a writer loses the connection. That is deliberate and documented.
- `ErrorCode::MessageInProgress` is appended to the enum (codes are never renumbered).
- Streaming through the C ABI (`wsrv_begin_message` and friends, M11) will wrap exactly this contract.
