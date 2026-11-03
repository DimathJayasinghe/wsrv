# ADR-0018: Handshake authentication hook (the application decides who may connect)

- Status: **Accepted** (2026-11-03; the project owner delegated the decision to the maintainer assistant)
- Date: 2026-11-03
- Deciders: project owner (delegated), maintainer assistant

## Context
Today the server accepts a WebSocket upgrade if the request is well formed and the `Origin`, subprotocol and path rules pass (`HandshakeNegotiator`). It cannot ask the *application* "is this user allowed in?". ADR-0016 section 7 left this out because the handshake driver is a pure function of bytes and answers immediately, while an application (PHP, over a socket, on another thread) answers later.

Applications need it for the normal web case: a session cookie or bearer token is sent with the upgrade request, and only the application can validate it. Without the hook the workaround is "accept everyone, then wait for an auth message and close", which lets anyone hold a connection slot and send messages before being authenticated, and it cannot answer with a proper HTTP 401/403.

Requirements: FR-H (handshake), T1 / T2 (handshake abuse), T16 (handlers). Constraint: the language runtime is never called from I/O threads (BINDINGS), so the decision must be asynchronous. It adds a connection phase, an IPC event and commands, and a C ABI event: structural, so an ADR first (CLAUDE.md).

## Options considered
1. **Asynchronous decision through the existing event pull** (this ADR). The connection waits after the handshake validated; the host gets a `Handshake` event, answers accept or reject by connection id; a timeout fails closed.
2. A synchronous C callback on the I/O thread. Rejected: runs the language runtime on an I/O thread, and a slow callback stalls every connection on that loop.
3. Validate in the daemon with a configured secret (for example verify a signed token). Useful as a later built-in, not general: the application owns the user store.
4. Keep "accept, then authenticate by message". Rejected as the only option for the reasons above; it stays available for applications that want it (the hook is opt-in).

## Decision

### 1. A new connection phase, between "validated" and "established"
`HandshakeDriver` is unchanged: it still decides everything it can decide alone (syntax, limits, Origin, subprotocol, version). When it would return `Accepted` and the hook is enabled, `Connection` instead:
- keeps the rendered 101 response and the negotiated subprotocol,
- enters `Phase::AwaitingDecision` and stops reading (a client may send frames right behind the head; they stay in the socket buffer, not in server memory),
- arms `handshake_decision_timeout` (default 3000 ms, range 100 to 60000), and
- hands a `HandshakeRequest` to the configured `IHandshakeDecider`.

`Rejected` outcomes (the driver refused) never reach the application.

### 2. What the application sees
`HandshakeRequest`: connection id, peer address, request path and query string, `Origin`, `Host`, the subprotocols the client offered and the one negotiated, and the values of headers named in a **configured allow-list** (`handshake_forward_headers`, default empty). `Cookie` and `Authorization` are secrets: they are forwarded only if the operator lists them, are never logged, and travel only over the local IPC socket or in-process queue. All values are copies with enforced size caps (a header is cut at 4 KiB and the event total at 16 KiB; an over-size value is a rejection with 431, never silent truncation of a credential).

### 3. The decision
`accept(conn, subprotocol?)` or `reject(conn, status)`.
- The subprotocol, if given, must be one the client offered; otherwise the call returns `INVALID_ARGUMENT` and the connection keeps waiting (the application may correct itself until the timeout).
- `status` is limited to 400, 401, 403, 404, 429, 500 and 503. The response has an empty body and `Connection: close`; a `Retry-After` is added for 429 and 503. No application text is ever echoed into the response (header injection and information leaks).
- The decision crosses threads only by `EventLoop::post` to the owning loop, through a `PendingHandle` (an id and the same guarded loop link `SessionHandle` uses), so a late or duplicate decision for a connection that is gone is a harmless `NO_SUCH_CONN`.
- **Fail closed.** No decision within the timeout, a full event queue, no decider, no application connected (in `wsrvd`), or a daemon that is shutting down all reject with 503. The hook never accepts by default. Timeout and rejection counts are reported as metrics (M10).

### 4. Bounds
Connections awaiting a decision count against `max_connections` like any other. An extra `max_pending_handshakes` (default 256, never above `max_connections`) caps how many may wait at once; the next one is rejected 503 immediately, so a flood of upgrade requests cannot queue unbounded work for a slow application. Each pending connection holds only its response string and a timer.

### 5. Wiring
- **Core:** `IHandshakeDecider` in `server/` (interface, composition-root injected); `Connection` phase and timer; `ServerConfig` fields above. The decider's implementation lives in `capi` (`HostBridge`), which pushes a `HostEventType::Handshake` event (sized and reserved like `Open`, so a decision-stage Close cannot be refused) and exposes `accept`/`reject` by id.
- **C ABI (additive, `wsrv.h` minor bump):** event type `WSRV_EV_HANDSHAKE` with a new `wsrv_handshake` view in the event, and `wsrv_handshake_accept()` / `wsrv_handshake_reject()`. Existing programs that do not enable the hook see no change.
- **IPC v0.2 (additive):** `EVENT` kind 6 `Handshake` (body carries the fields of section 2 as length-prefixed strings), command frames 10 `HANDSHAKE_ACCEPT` and 11 `HANDSHAKE_REJECT`, each answered by a `STATUS`. The daemon enables the hook with `ipc_handshake_hook = true`; clients that do not send it are never asked. HELLO_OK advertises the minor version so a client can tell.
- **PHP:** an `authenticate` callback on `Worker` returning accept or reject.

### 6. Out of scope
Built-in token validation (option 3), per-IP rate limiting of failed attempts (M10), and sticky decisions for reconnects.

## Slices
1. `server/`: phase, timer, bounds, `IHandshakeDecider` interface, fake-decider tests (accept, reject, timeout, duplicate and late decisions, flood, shutdown while pending, bytes arriving while waiting).
2. `capi`: `HostBridge` implementation, event type, accept/reject, C ABI addition, C consumer test, `wsrv.symbols` update.
3. IPC v0.2 and `wsrvd`: frames, fail-closed when no application, docs, fuzz target update.
4. PHP `authenticate` callback, interop test with a real cookie check.

## Consequences
+ Real authentication with proper HTTP status codes, before any WebSocket resource is used.
+ Same queue, backpressure and thread rules as every other event; no runtime on I/O threads.
- A new connection phase and a timer: one more state in `Connection` to test and fuzz (the handshake fuzz target gains a decider that answers late).
- Operators who list `Cookie` or `Authorization` accept that those secrets pass through the host process; the default forwards nothing.
- Fail-closed means an unresponsive application makes new connections fail with 503; that is the intended signal, not a bug.

## Amendments

### Amendment 1 (2026-11-03, slice 1 design review): what the code pass settled
1. **Bytes behind the request head.** The read buffer is shared by all connections of a loop, so bytes that arrive in the same read as the head cannot stay there while the application thinks. They are copied into an owned buffer (at most one read, 16 KiB) and fed to the session right after the 101. Section 4's "holds only its response string and a timer" is therefore "plus at most 16 KiB of pipelined bytes": worst case `max_pending_handshakes` x 16 KiB (4 MiB at the default). Bytes that arrive later stay in the kernel (reading is off).
2. **Reading off is not "paused reads".** `pause_reads()` belongs to the application's session. The waiting phase drops Read interest on its own flag; `RDHUP` stays registered so a client that leaves is noticed at once, not at the timeout. On accept the registration is changed for real (Read added back), because an edge-triggered registration only reports data again after a mask change.
3. **The decision is not a session command.** It enters the loop through its own table method and acts only in `AwaitingDecision`; in any other state it is dropped silently (late, duplicate, unknown id). The synchronous errors of section 3 (`INVALID_ARGUMENT` for a subprotocol the client did not offer, a status outside the allowed set) are produced on the caller's thread, where `PendingHandle` holds a copy of the offered subprotocols. `NO_SUCH_CONN` for a connection that is gone comes from the host's own bookkeeping (slice 2), not from the core.
4. **Accepting with another subprotocol** re-renders the 101, so the handshake outcome now carries the accept key.
5. **`max_pending_handshakes` is global** (one atomic shared by all loops, like the connection cap), released through an RAII slot exactly once on every exit.
6. **`Retry-After` is 5 seconds** for 429 and 503, a constant for now (not configurable).
7. **Shutdown while waiting** answers 503 (section 3 says shutdown rejects), instead of the silent drop used for a handshake that is still being read.
8. **"Never above `max_connections`" is a clamp, not a refusal.** The default (256) is larger than many small servers' `max_connections`, so validation only requires `0 < max_pending_handshakes <= 1048576` and the effective bound is `min(max_pending_handshakes, max_connections)`.

### Amendment 2 (2026-11-03, slice 2: the host side)
1. **A `Handshake` event is not followed by a `Close`.** Section 5 said the event is "reserved like `Open`", but a refused upgrade never becomes a connection, so the host must see no `Close` for it. The queue treats `Handshake` like `Text` (it needs room for itself, nothing is reserved); a full queue answers the server with an error and the upgrade gets 503. An accepted upgrade then produces the usual `Open` (reserving its `Close`) with the same id.
2. **Host-side bookkeeping.** `HostBridge` keeps a map id -> `PendingHandle`, filled before the event is pushed (the host may answer the instant it sees it) and emptied by the first successful answer, by a failed push, and by `IHandshakeDecider::withdrawn(id)` (new: the server calls it when it ends the wait itself: timeout, client gone, shutdown). So a second or late answer is `NO_SUCH_CONN`, not a silent success. A mistaken answer (`INVALID_ARGUMENT`) keeps the entry: the wait goes on.
3. **One codec for the question.** The fields of section 2 travel in `HostEvent::data` as a length-prefixed blob (`capi/handshake_blob.hpp`), decoded by the C layer into `wsrv_handshake` and, in slice 3, by `wsrvd` into the IPC body. It never trusts a length: any truncation, overrun or trailing byte makes the decode fail.
4. **C ABI 0.2.** `wsrv_event` gained a trailing `handshake` pointer, so `sizeof` grew and a 0.1 caller (smaller `struct_size`) is refused with `INVALID_ARGUMENT`: loud, and allowed while the major version is 0. Settings: `handshake_hook`, `handshake_forward_header` (repeatable), `handshake_decision_timeout_ms`, `max_pending_handshakes`.

