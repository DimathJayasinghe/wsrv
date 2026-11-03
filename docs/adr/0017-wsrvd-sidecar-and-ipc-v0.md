# ADR-0017: `wsrvd` sidecar and IPC protocol v0

- Status: Accepted (project owner, in chat, 2026-11-02: "we can go with option 1")
- Date: 2026-11-02
- Deciders: project owner (to accept)

## Context
ADR-0010 chose two integration modes sharing one core: embedded (done: ADR-0016, `libwsrv.so`) and sidecar. The sidecar exists for hosts that cannot hold a native thread or a long-lived socket in their own process, above all PHP-FPM / Laravel / Symfony (BINDINGS section 4.2), and for any language that can open a Unix socket. The plan (M11) requires `wsrvd`, `docs/IPC_PROTOCOL.md`, an IPC fuzz target, and documented fork and signal rules.

The sidecar adds new surface that can be attacked from the local machine (anyone who can open the IPC socket can send messages as the application) and a second parser fed with untrusted bytes. It adds a module, a binary and a wire format, so CLAUDE.md requires an ADR first. Requirements: FR-S3, FR-S4. Threats: T3, T16, BINDINGS section 7 "Security of sidecar IPC".

## Options considered
1. **Unix domain socket, length-prefixed binary frames, one consumer connection** (this ADR). Simple, fast, no HTTP stack, permissions from the filesystem, peer credentials from the kernel.
2. JSON lines over a Unix socket. Easiest for clients, but binary payloads need escaping or base64, parsing cost is high, and a JSON parser is new attack surface.
3. HTTP/gRPC webhooks. Works for FPM without a daemon-side client, but adds an HTTP client and a dependency, and loses ordering and backpressure.
4. Shared memory ring buffers. Fastest, hardest to make safe across languages; premature.

## Decision

### 1. Option 1, with the protocol in a sans-I/O module
- New module `src/ipc/` (depends on `base` only, like `protocol/`): message types, a frame encoder and an incremental decoder with strict bounds. It includes no OS headers, so it is fuzzable (`fuzz_ipc_frame`) and shared by `wsrvd` and any future C++ client.
- `wsrvd` is a composition root in `apps/wsrvd/` (concrete types allowed there). It links `capi` (`EmbeddedServer`) and `ipc`, plus `net` for its own event loop on the main thread. The WebSocket I/O threads are still the ones `EmbeddedServer` already owns; IPC I/O runs on the daemon's main thread. Nothing in `server/` changes.
- ARCHITECTURE section 2.1 gains `ipc -> base` and the `wsrvd` application edges; `check_includes.py`, the `module_graph` test and `tests/module_graph.expected` follow.

### 2. Wire format (v0)
Every frame: `u32 length` (big-endian, bytes after this field), `u8 type`, `u8 flags` (reserved, must be 0), then the body. Integers are big-endian; text is UTF-8 with explicit lengths; nothing is NUL-terminated.
- Maximum frame length is a setting (`ipc_max_frame`, default `max_message_size + 64 KiB`, hard cap 64 MiB). A larger declared length closes the IPC connection before any buffer is sized from it.
- Client to daemon: `HELLO(version, auth token)`, `SEND_TEXT(conn, bytes)`, `SEND_BINARY(conn, bytes)`, `CLOSE(conn, code, reason)`, `PING`.
- Daemon to client: `HELLO_OK(version, limits)`, `EVENT(type, conn, close_code, clean, data)` with the same five event types as `wsrv_event`, `STATUS(request id, status)` for every command, `PONG`.
- Commands carry a client-chosen `u32 request id`; the daemon answers each with a `STATUS` using the same codes as `wsrv_status`, so the mapping table is shared with `capi`. A command is "accepted" in the sense of ADR-0016 section 5, not delivered.
- Version: major in `HELLO`; a different major is refused with a status and the connection closed. Unknown frame types are a protocol error (close), never skipped, because skipping would desynchronise length prefixes.

### 3. One consumer, bounded, with backpressure end to end
- v0 accepts **one** application connection at a time (the single-consumer rule of `wsrv_poll`). A second connection is refused with a status. A worker pool needs a routing rule and is a separate ADR.
- The daemon moves events from `EmbeddedServer::poll` to the IPC socket. Its write buffer is bounded; when it is full it **stops polling**, so the existing event-queue high-water mark pauses WebSocket reads (ADR-0016 section 4). No new drop path exists. If the application stops reading for longer than `ipc_stall_ms`, the daemon closes the IPC connection and shuts connections down with 1001 (documented, not hidden).
- If the application disconnects, the daemon stops accepting new WebSocket connections and closes existing ones with 1012 (Service Restart) after `shutdown_grace_ms`, then waits for a new application connection. Open and Close events for connections the application never saw are not replayed; the application receives a fresh `HELLO_OK`, and connection ids are never reused, so stale ids give `NO_SUCH_CONN`.

### 4. Local security
- The socket path is configured explicitly; the daemon creates it with mode `0600` (owner only), refuses a path whose parent directory is group- or world-writable, removes only a socket file that it owns and that no process answers on, and never follows a symlink at that path.
- After accept, `SO_PEERCRED` is read; the peer uid must equal the daemon's uid (`ipc_allow_uid` was dropped, see 4a).
- `HELLO` carries a shared token (setting `ipc_token`, compared in constant time) as a second factor for deployments that share a uid between services. No token configured is allowed only with the uid check on; this is logged at startup.
- The daemon never executes anything it receives and never interprets payloads.
- Signals: `wsrvd` is a program, so unlike the library it installs `SIGINT/SIGTERM` handlers through the existing `SignalWatcher` and shuts down gracefully (`wsrv_server_stop` semantics). Fork rules for the library are unchanged; `wsrvd` itself never forks after threads start.

### 4a. Amendment (2026-11-02, project owner, in chat): three clarifications found while designing slice 2
- **No `ipc_allow_uid` in v0.** On Linux `connect()` to a pathname socket needs write permission on the socket file, so with mode `0600` only the owning uid can ever connect and a list of extra uids could never work. The socket stays `0600`; the `SO_PEERCRED` check stays as defence in depth (peer uid must equal the daemon's uid). Sharing the socket between uids is a later, separate decision (a configurable mode or group) and would be additive.
- **A refused `HELLO` is answered with `STATUS` with request id 0** (`Hello` carries no id). Id 0 is reserved: clients number commands from 1, and the daemon (slice 3) answers a command with id 0 as `WSRV_ERR_INVALID_ARGUMENT`, so a status with id 0 can only mean "about the handshake". The codec itself does not judge ids. `wsrv.h` is not changed: a wrong version or a wrong token maps to `WSRV_ERR_INVALID_ARGUMENT`, a second consumer to `WSRV_ERR_BACKPRESSURE` ("the single slot is full, try later"). The connection is closed after the status.
- **`HELLO` deadline.** A peer that connects and never sends `HELLO` would hold the single consumer slot forever, so the daemon closes an IPC connection that has not completed `HELLO` within `ipc_hello_timeout_ms` (default 5000). Before `HELLO` is accepted the decoder is bounded to the largest legal `HELLO` frame, not to `ipc_max_frame`, so an unauthenticated peer cannot make the daemon gather a large body.

### 4b. Portability (owner requirement, 2026-11-02)
The library is meant to run on Linux, macOS and Windows. Per PROJECT_PLAN and ADR-0004 only Linux is built in v1 (macOS `KqueuePoller` and Windows are M13). The rule for all new code until then: OS-specific calls live only in `net/` behind `ISyscalls`, and anything that differs per OS is behind a small interface (here: peer credentials, which are `SO_PEERCRED` on Linux, `LOCAL_PEERCRED`/`getpeereid` on macOS and a named-pipe or token check on Windows). `src/ipc/` is already free of OS headers. No new Linux-only code appears outside `net/`.

### 4c. Amendment (2026-11-03, project owner, in chat: "go with the recommended decisions"): pump, disconnect and stall, found while building slice 3
- **No application connected: hold, do not refuse.** `EmbeddedServer` has no way to pause accepting, and adding one would be a structural change to `server/`. Instead the daemon takes no events while nobody is connected. WebSocket handshakes complete, and clients wait behind the existing event-queue high-water pause until an application arrives, which then receives everything in order. Nothing is lost. (A true pause-accept remains possible later with its own ADR.)
- **Application disconnects:** the connections it had been told about become *stale*. Their further events are discarded (the only discard in the daemon) and commands for them get `NO_SUCH_CONN`; after `shutdown_grace_ms` they are closed with 1012. Connections it never saw are served by the next application. Delivery is at most once across a disconnect.
- **Stall:** `ipc_stall_ms` defaults to 10000 (range 100 to 600000). The clock starts when a write blocks with data pending, restarts on any byte of progress, and is cancelled when the buffer is empty. On expiry the link is closed and the stale connections are closed with 1001 immediately (no grace).
- **Backpressure in both directions:** above high water (one max frame of unsent output) the daemon stops polling the backend and also stops reading commands, so answers cannot grow the buffer. The stall timer is the only kill path for a slow application.
- **Frame limit:** an explicit `ipc_max_frame` smaller than `max_message_size` + 18 bytes is refused at startup, so no message the WebSocket side accepts can be unforwardable at runtime.
- Request id 0 is answered `INVALID_ARGUMENT` and never executed (as announced in 4a).

### 5. Out of scope for v0
Publish-only clients for FPM (`publish`/`subscribe` need the M10 broker; the frame types are reserved and absent), multiple application connections, TCP transport, TLS on the IPC link, at-least-once delivery, the handshake authentication hook (same blocker as ADR-0016 section 7), and the PHP client (M12).

## Slices
1. `ipc/` module: frame types, encoder, incremental decoder, unit tests, `fuzz_ipc_frame` with a committed corpus. Pure, no sockets.
2. `wsrvd` skeleton: configuration file/flags, IPC listener with socket hygiene and peer credentials, `HELLO`, a loopback integration test over a real Unix socket.
3. Event pump and commands with backpressure; adversarial tests (oversized length, split frames, slow application, application disconnect); a C++ test client. `docs/IPC_PROTOCOL.md` is written with this slice. Lesson 16.

## Consequences
+ PHP-FPM and any language with sockets can use the server with a client of a few hundred lines; crashes are isolated from the daemon.
+ Same queue, same backpressure, same status codes as the embedded mode; nothing is decided twice.
- One more hop and one more process to deploy.
- A new binary parser on local input: mitigated by length bounds, a sans-I/O design and fuzzing, but it is real attack surface.
- v0 serves one application connection; horizontal scaling of the application needs the follow-up ADR.
- Revisit if the framing proves too costly to implement in client languages, or if FPM needs publish-only clients before M10 (then an additive frame type and an ADR amendment).
