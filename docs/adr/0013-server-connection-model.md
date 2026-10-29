# ADR-0013: Server connection model, handler contract and teardown

- Status: Proposed
- Date: 2026-10-29
- Deciders: project owner (to accept)

## Context
M6 adds the `server` module: the first code that joins `net` (sockets, loop) to `protocol`/`http` (bytes, rules). Four decisions shape everything above it and are hard to change later (the C ABI of M11 wraps them): where the public API lives, what a handler may do inside a callback, how a connection dies without use-after-free, and what happens to a client that does not read. Requirements: FR-S1..S6, FR-C2, FR-C5, FR-H8; edge cases EC-A01..A06, EC-N02, N07..N09, EC-H19..H21, EC-C18; threats T2, T3, T16.

## Decisions

### 1. API location: internal headers first, `include/wsrv/` at 1.0-rc
Public headers cannot include `src/base` (`Result`, `Error`, `CloseCode`, `ByteView`), and duplicating those types would fork the error model (ADR-0006). The handler API (`IWebSocketHandler`, `Session`, `ServerBuilder`) is therefore written in `src/server/` in M6 and treated as **unstable**. At M10 (1.0-rc) the stabilised surface is promoted to `include/wsrv/` together with the base types it needs; at M11 the C ABI becomes the only cross-language contract. Revisit trigger: an M7/M10 consumer outside the repo needs the C++ API earlier. No `CHANGELOG.md` entry is required until a header is published. This deviates from the "users see `include/wsrv/` only" sentence in ARCHITECTURE section 1; that section is updated in the same PR.

### 2. Handler contract (EC-A01..A06)
- Callbacks run on the connection's loop thread, never block, and may call `Session` methods (send, close) except `send` after close returns `Closing` (EC-A03).
- `string_view`/`span` arguments are valid for the call only. Echoing them (`send_text(view)`) is safe because `ProtocolSession::send_*` copies into its queue before returning.
- **Exception barrier (FR-S6):** every handler call is wrapped in one `try`/`catch (const std::exception&)` plus one `catch (...)`; both log (sanitised) and close the connection with 1011. The catch-all does not swallow: it converts to a close and a log line. This is the only catch-all in the module.
- **Re-entrancy (EC-A06):** `Session::close()` called from a callback only records the request and flags the connection; the work (flush, state change) runs after the callback unwinds. `on_close` is delivered exactly once, only if `on_open` ran, and never from inside another callback.

### 3. Teardown without use-after-free
A `Connection` is an `IEventHandler` owned by a per-loop table. It can decide to die from inside its own `on_event`, so it must not be destroyed there. It marks itself `Closed`, unwatches, closes the transport, and asks the table to retire it; the table destroys retired connections at a **safe point after the current event batch** via a new `EventLoop::defer(Task)` (loop-thread-only, FIFO, bounded by `max_watches`, runs before the next poll). `post()` is not used: it is bounded for cross-thread producers and may return `BufferFull`, which a teardown path cannot tolerate.

### 4. Slow consumer (EC-N09, T3)
The outbound queue bound is `OutboundConfig::max_queued_bytes`. When `send_*` returns `Backpressure`, the application sees the error value; the connection's *policy* is `SlowConsumerPolicy::Close` (default, close 1008) or `DropMessage`. A write stall timer (`write_stall_timeout`, default 30 s, queue non-empty and no byte accepted by the kernel) closes the transport without waiting for a Close handshake that can never complete (the peer is not reading). If the data queue is full, `close()` still works because Close has its own slot in the scheduler (ADR-0007).

## Consequences
+ Teardown ordering is one rule, testable with ASAN. Handlers cannot crash the loop. The unstable API can change freely until 1.0-rc.
- `defer()` is a small `net` addition (reviewed as such). The catch-all is a deliberate, documented exception to "no catch(...)"; it never swallows silently.
- Per-IP caps, handshake rate limits and the global buffered-bytes budget (`LimitsEnforcer`) are **not** in M6; only the global connection cap is. They land in M10.
