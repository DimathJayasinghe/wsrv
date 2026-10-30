# ADR-0016: C ABI v0 (embedded, pull model) and read pause

- Status: Accepted (project owner, in chat, 2026-10-30)
- Date: 2026-10-30
- Deciders: project owner (to accept)

## Context
ADR-0009 chose a hand-designed C ABI with a pull-based event queue as the single binding surface, and ADR-0010 chose embedded plus sidecar modes. BINDINGS.md section 3 sketches `wsrv.h`. This ADR fixes the first buildable slice (milestone M11, "C ABI first": owner decision 2026-10-30) so that PHP can reach the server through FFI on CLI scripts, and so that the `wsrvd` sidecar (next ADR) can reuse the same event and command queues. It touches a new module, a public surface that must stay stable, one thread, and one new server capability (read pause), so it needs an ADR (CLAUDE.md). Requirements: FR-S3, FR-S4; hard rule "the language runtime is never called from I/O threads" (BINDINGS section 1). Threats: T3, T16, risk "ABI break" and "event queue overload when the host is slow" (BINDINGS section 7).

## Decisions

### 1. A new module `capi/`, depending on `server` (plus `base` and `net`)
`src/capi/` builds `libwsrv.so` (and a static twin for tests). It may include `server/` headers, and `base/` and `net/` ones for `Result`, `Fd` and the syscall seam; it must not reach `protocol/` or `http/` directly, and nothing includes it. `tools/check_includes.py`, the `module_graph` test and ARCHITECTURE section 2.1 gain the `capi -> base, net, server` edge. The only public header is `include/wsrv/wsrv.h`: pure C99, opaque handles, fixed-width integers, no C++ types. The C++ headers stay in `src/server/`. Build flags: `-fvisibility=hidden`; exported functions are marked `WSRV_API`. At the boundary every function catches `std::bad_alloc` and `std::exception` and returns a status; no exception crosses it and no `catch(...)` is used.

### 2. Runtime shape
- `wsrv_server_create` builds the C++ `Server` on the caller's thread, so configuration problems are reported synchronously. The `ConfigError` text is copied into a **caller-supplied fixed buffer** (`wsrv_error { int code; char message[128]; }`); the library never returns memory the caller must free for an error.
- `wsrv_server_start` starts **one I/O thread** that calls `Server::run()`. That thread is loop 0; further loops (`threads` config) are started by the server as in M7. This is allowed because `EventLoop` re-records its owner thread at every `run_once` (ADR-0013); a test covers create-on-one-thread, run-on-another.
- The library installs **no signal handlers** (BINDINGS section 7). `wsrv_server_install_signal_handlers` does not exist; the host owns signals.
- `wsrv_server_stop(server)` posts `request_shutdown()` (any thread, idempotent); the grace period is the config key `shutdown_grace_ms`. This drops the `grace_ms` parameter shown in the BINDINGS sketch: a parameter cannot be removed later, a config key can be added. The sketch is updated with this ADR. `wsrv_server_destroy` stops if needed, forces after the grace, joins the thread, and frees everything.
- **Fork safety:** the creating pid is stored; every function called from another pid returns `WSRV_ERR_WRONG_PROCESS`. A server must be created after `fork()`.

### 3. Events are copies in one bounded queue
The per-connection handler (`IWebSocketHandler`) built by the library's factory runs on a loop thread and **copies** each event (open, text, binary, close, error) into one process-wide `EventQueue`; the host pops with `wsrv_poll(server, &event, timeout_ms)` and frees with `wsrv_event_release`. `wsrv_event` carries type, connection id, data pointer and length (valid until released), close code and a `clean` flag. An OPEN event's data is the peer address as UTF-8 `host:port`; subprotocol and header snapshots arrive with the handshake hook (out of scope here). Texts are UTF-8-valid (FR-F7), so bindings may decode without checking. `wsrv_poll` is single-consumer: one host thread at a time (documented; it is not locked against two pollers).

The queue is multi-producer (loops), single-consumer (host), bounded in events and bytes. It uses **one mutex, in `capi/`**, held for one push or pop, like `LoopLink` and the `EventLoop` task queue (lesson 13): the rule "no locks in connection or protocol code" is about those classes, and neither gains a lock. `wsrv_server_event_fd` returns an `eventfd` that is readable exactly while the queue is non-empty: the producer that makes it non-empty writes it, and the consumer that empties it reads it, **both under the queue mutex**, so a wake-up cannot be lost between "look" and "sleep" (the trap from lesson 13 section 4). The host may poll the descriptor with its own loop (ReactPHP, Revolt, `stream_select`).

### 4. Overflow action: pause reads, then a hard cap
Every bounded buffer needs a defined overflow action (CLAUDE.md), and BINDINGS section 3 chose backpressure over silent drops.
- **High-water** (queued events or bytes at 3/4 of the room left once every accepted Open's Close slot is set aside): the producing connection **pauses reading** (new `Session::pause_reads()`, loop-thread-only; it drops `EPOLLIN` from the connection's interest). TCP flow control then slows the peer.
- **Resume:** when the host has drained the queue to the **low-water mark** (1/2 of that room; an empty queue always qualifies), the library posts `resume_reads()` to each paused connection through its `SessionHandle`. Resuming re-arms `EPOLLIN` from a mask that does not contain it, so the kernel reports data that is already waiting. This is the edge-triggered re-arm trap found in ADR-0015 / lesson 12 and gets a **real-socket test** (a fake host cannot show it).
- **Hard cap** (the configured limit): a delivered message cannot be refused (handlers return `void`), and one read batch of up to 64 KiB (`max_bytes_per_event`) plus a completing message of up to `max_message_size` can arrive after the pause. So the limit is checked at push, under the mutex: an event that would exceed it is dropped and its connection is closed with **1013 (Try Again Later)**. The queue size is therefore never above the limit, whatever the number of connections.
- Pressure counts only what is queued, never the reserved Close slots: otherwise idle connections alone could keep the queue "pressured" for ever and pause everyone else with nothing to drain (found by the automated security review of slice 2b). `HostBridge::poll()` also resumes every parked connection whenever it finds the queue empty and not pressured, as a safety net. The number of announced connections is itself bounded by the queue (an Open that cannot reserve its Close is refused with 1013).
- A paused connection is still subject to idle and heartbeat timeouts (it cannot answer a Ping while not reading), so a host that stops polling for longer than those timeouts loses its connections. That is the intended bound; it is documented, not hidden.

### 5. Sending by connection id from the host thread
The C API only has a `wsrv_conn_id`. The library keeps `unordered_map<ConnectionId, SessionHandle>` behind its mutex: inserted in `on_open` (loop thread), erased in `on_close`. `wsrv_send_text/binary` and `wsrv_close` look up the handle and call it (ADR-0014 section 4). A stale or unknown id returns `WSRV_ERR_NO_SUCH_CONN`; a full loop task queue returns `WSRV_ERR_BACKPRESSURE`; "accepted" still means queued for the loop, not delivered. Status codes map from `ErrorCode` in one table, tested for completeness like `kAllCodes`.

### 6. Configuration
`wsrv_config_set_str(cfg, key, value)` validates and fails with `WSRV_ERR_INVALID_CONFIG` for an unknown key or bad value; keys are listed in the header and never removed. v0 keys: `listen_host`, `listen_port`, `threads`, `allow_origin` (repeatable), `allow_missing_origin`, `max_message_size`, `max_connections`, `shutdown_grace_ms`, `event_queue_bytes`, `event_queue_events`. The default Origin policy is unchanged (reject without an allow-list).

### 7. Out of scope for v0 (additive later)
- `wsrv_publish` / `wsrv_subscribe`: need the M10 broker.
- The handshake authentication hook (`WSRV_EV_HANDSHAKE`, accept/reject): needs an asynchronous `HandshakeDriver`. Until then, host-side authentication is Origin allow-list plus an application-level token message.
- **Streaming in both directions.** ADR-0015 says the C ABI "will wrap exactly this contract", but `MessageWriter` is loop-thread-only and a C caller is not on the loop thread. A host-side writer needs per-connection stream state living on the loop and driven by posted commands, with its own backpressure signal in the event stream. That is a separate ADR before the streaming slice; v0 sends whole messages.

### 8. ABI checking without a new tool dependency
`abi-compliance-checker` would be a new tool and needs its own ADR. v0 uses: (a) `include/wsrv/wsrv.h` compiled as C99 with `-Wpedantic -Werror`; (b) a checked-in `include/wsrv/wsrv.symbols` compared against `nm -D libwsrv.so` in CI (an unexpected or removed export fails the build); (c) a **C-only consumer** (`tests/capi/consumer.c`) that links the shared library, starts a server, completes a WebSocket handshake with a raw socket, echoes a message, and shuts down. The ABI checker is revisited for 1.0-rc.

### 9. The binding rule: bindings translate, the core decides
The owner's requirement (2026-10-30): PHP cannot be expected to manage sockets and connection state well, and the same core must serve other platforms (C#, Java/Spring, Node, Python, Go...). So every behaviour that could be got wrong is implemented **once, below the language line**, and a binding only converts types and calls:
- **In the core / `capi` (C++, tested once):** protocol rules, UTF-8 validation of both directions, message and queue limits, backpressure and overload policy, connection ids and their lifetime, "exactly one Close for every Open", ordering, send-to-a-dead-connection safety, fork and shutdown behaviour, error codes and their mapping.
- **In a binding (thin, per language):** calling the C functions, turning `wsrv_event` into the language's object, integrating the `event_fd` with that language's event loop, memory release at the FFI boundary. A binding contains no protocol, buffering or state-machine code. If a binding needs a new behaviour, it is added to `capi` first and benefits every language.
- **Review test:** a bug in connection handling must be fixable by changing C++ only; a binding that needs a workaround for a socket or state edge case is a bug in `capi`.
`HostBridge` (slice 1) is this rule in code: one class owning the event queue and the id-to-handle map, with no C types.

## Slices
1. Adapter and `EventQueue` in C++ (module `capi/`, no C header yet), unit tests, TSan.
2. `Session::pause_reads` / `resume_reads` in `Connection`, with the real-socket resume test (a server-layer change, so it gets its own PR).
3. `wsrv.h`, the `extern "C"` shim, `libwsrv.so`, symbol list, C consumer test. Lesson 15 follows.

## Consequences
+ Any language with FFI can serve WebSockets without the runtime ever running on an I/O thread.
+ One queue and one command path serve both this embedded mode and the sidecar.
+ The queue size has a real, computable worst case: the configured limit.
- Each event is copied once at the boundary (ADR-0009 accepted this).
- A `Session`-level read pause is a new capability in the server layer, with an edge-triggered re-arm subtlety.
- A slow host eventually costs its connections (1013, or timeouts while paused). That is deliberate: the alternative is unbounded memory.
- `wsrv_server_stop` differs from the BINDINGS sketch (no `grace_ms` parameter); BINDINGS.md is updated in the same PR.
