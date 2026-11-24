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
- `wsrv_server_create` only holds configuration and builds the event queue; it never touches the network. Settings are text key/value pairs (`wsrv_config_set`), each checked when set (known key, clean number or boolean); the combination is validated once, by `ServerBuilder::build()`, at start.
- `wsrv_server_start` spawns **one I/O thread** and **waits until the server is listening or has failed**. That thread builds the `Server`, runs it (it is loop 0; further loops come from the `threads` setting as in M7) and destroys it: the loops assert that their loop-thread API is used on the owning thread (ADR-0013), so build, run and destroy cannot be split across the host thread and the I/O thread. Because `start` waits for the answer, a bind failure (`AddressInUse`) or an unusable setting is still reported synchronously, with a reason in a **caller-supplied fixed buffer** (`wsrv_error { int code; char message[128]; }`); the library never returns memory the caller must free for an error. (An earlier draft built the server on the host thread; the thread assertions made that unworkable.)
- The library installs **no signal handlers** (BINDINGS section 7). `wsrv_server_install_signal_handlers` does not exist; the host owns signals.
- `wsrv_server_stop(server)` posts `request_shutdown()` (any thread, idempotent, fine before start or after the end); the grace period is the setting `shutdown_grace_ms`. This drops the `grace_ms` parameter shown in the BINDINGS sketch: a parameter cannot be removed later, a setting can be added. The sketch is updated with this ADR. `wsrv_server_destroy` stops if needed, waits for the grace period, joins the thread and frees everything. Events stay readable after the server ended, so the host still sees every Close.
- **Fork safety:** the creating pid is stored; every function called from another pid returns `WSRV_ERR_WRONG_PROCESS` (`ErrorCode::WrongProcess`). A server must be created after `fork()`.
- All of the above is one C++ class, `EmbeddedServer` (module `capi/`), tested without any C: the C functions are a one-line translation each (section 9).

### 3. Events are copies in one bounded queue
The per-connection handler (`IWebSocketHandler`) built by the library's factory runs on a loop thread and **copies** each event (open, text, binary, close, error) into one process-wide `EventQueue`; the host pops with `wsrv_poll(server, &event, timeout_ms)` and frees with `wsrv_event_release`. `wsrv_event` carries type, connection id, data pointer and length (valid until released), close code and a `clean` flag. An OPEN event's data is the peer address as UTF-8 `host:port`; subprotocol and header snapshots arrive with the handshake hook (out of scope here). Texts are UTF-8-valid (FR-F7), so bindings may decode without checking. `wsrv_poll` is single-consumer: one host thread at a time (documented; it is not locked against two pollers).

The queue is multi-producer (loops), single-consumer (host), bounded in events and bytes. It uses **one mutex, in `capi/`**, held for one push or pop, like `LoopLink` and the `EventLoop` task queue (lesson 13): the rule "no locks in connection or protocol code" is about those classes, and neither gains a lock. `wsrv_server_event_fd` returns an `eventfd` that is readable while the queue is non-empty and its wake-up has been paid (A3: a message's wake-up is paid at the end of the loop iteration that queued it): the producer that makes it non-empty writes it, or owes it (A3), and the consumer that empties it reads it, **both under the queue mutex**, so a wake-up cannot be lost between "look" and "sleep" (the trap from lesson 13 section 4). The host may poll the descriptor with its own loop (ReactPHP, Revolt, `stream_select`).

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
3. `wsrv.h`, the `extern "C"` shim, `libwsrv.so`, symbol list, C consumer test. Lesson 15 follows. *(3a: `EmbeddedServer` and configuration. 3b: the header, shim, shared library, symbol list and consumer.)*

Implementation notes for slice 3b (no decision above changed):
- The export list is enforced by the linker with an **anonymous** version script generated from `wsrv.symbols` (`{ global: ...; local: *; };`). `-fvisibility=hidden` plus `--exclude-libs` alone still leaked libstdc++ template typeinfo, because those symbols carry default visibility from the standard headers. No symbol versioning (`name@@VERSION`) is introduced: that would be an ABI decision, so it needs its own ADR if wanted.
- `wsrv_status_name` and the C-only code live in the shared library only; the status table (`to_c_status`) lives in `wsrv_capi` so the C++ tests can walk it.
- `wsrv_server_destroy` returns `void`, so the C API cannot report `WRONG_PROCESS` there (section 2 says every function does).

## Consequences
+ Any language with FFI can serve WebSockets without the runtime ever running on an I/O thread.
+ One queue and one command path serve both this embedded mode and the sidecar.
+ The queue size has a real, computable worst case: the configured limit.
- Each event is copied once at the boundary (ADR-0009 accepted this).
- A `Session`-level read pause is a new capability in the server layer, with an edge-triggered re-arm subtlety.
- A slow host eventually costs its connections (1013, or timeouts while paused). That is deliberate: the alternative is unbounded memory.
- `wsrv_server_stop` differs from the BINDINGS sketch (no `grace_ms` parameter); BINDINGS.md is updated in the same PR.

## Amendments

### A1 (2026-11-19, proposed): three limit keys, so a C consumer can run the conformance suite

**Problem.** M11's exit criterion is "the C consumer passes Autobahn". The suite needs three limits that the library keeps low by default (FR-F6, FR-F10): one frame of up to 16 MiB, a 4 MiB message in 65 536 fragments, and bursts of small frames (`tests/autobahn/EXCLUDED.md`). The C++ echo raises them through `ServerConfig`; the C configuration has no key for them, and `max_message_size` alone moves neither the frame limit, the fragment limit nor the frame rate. Without keys a C host cannot serve those cases at all, and an embedder with a legitimate need (large uploads, a trusted batch client) has no way to say so.

**Decision.** Three additive text keys, parsed and range-checked in `EmbeddedConfig` when they are set (so a bad value fails `wsrv_config_set` at once, not at start):

| Key | Sets | Default (unchanged) | Accepted |
|-----|------|---------------------|----------|
| `max_frame_payload` | `session.parser.max_frame_payload` | 1 MiB | 1 .. 64 MiB (`kMaxConfiguredFrame`, the ceiling `Session` already enforces) |
| `max_fragments` | `session.assembler.max_fragments` | 1024 | 1 .. 16 777 216 |
| `max_data_frames_per_second` | `session.max_data_frames_per_second` | 10 000 | 1 .. 10 000 000 |

- `Session` itself has no ceiling for the last two (`max_fragments` only must be non-zero; `0` for the frame rate means "off"). The C layer adds the ceilings and refuses `0`, because SECURITY_DESIGN section 3 says there is no "unlimited" value and a C host should not be able to switch the flood guard off by typo. The C++ API keeps its wider range; this is a deliberate asymmetry, recorded here.
- A malformed or out-of-range value fails `wsrv_config_set` with `WSRV_ERR_INVALID_CONFIG` and changes nothing.
- Defaults do not change, and no key lowers a hard ceiling. Raising a limit is the embedder's explicit act, named in its configuration.
- The header lists the keys and says what each costs: a larger frame limit raises the worst-case buffer per connection, a larger fragment limit lets one message cost more bookkeeping, a larger rate limit weakens the flood guard (EC-F31).
- `WSRV_ABI_VERSION`: the minor number goes from 3 to 4. No symbol changes; the version tells a binding that the keys exist, so it can probe with `wsrv_abi_version()` instead of trying a key and parsing an error.
- The sidecar's text configuration is the same key table, so `wsrvd` gains the keys with no extra code.

**Rejected.** (a) Deriving the three from `max_message_size`: hides a coupling and cannot express "big frames, few fragments". (b) A C-side "autobahn profile" key: puts test policy in the product ABI. (c) Running a reduced case set from C: the exit criterion would then not mean what it says.

**Consequences.** + The C consumer can run the same 517 cases as the C++ echo. + Embedders get the knobs the C++ API already has. - Three more keys to keep forever; each is a limit, so each is also a way to configure an unsafe server. The header text and OPERATIONS say so.

### A2 (2026-11-23): the default event queue follows `max_connections`

**Problem.** `EventQueue` reserves, when a connection opens, a slot (and `kReservedCloseBytes`) for that connection's Close event, and keeps it until the connection ends, so a Close can never be refused (section 4). The default queue was 4096 events, whatever `max_connections` said (default 10 000). A server therefore held at most 4095 live connections: the 4096th was accepted and upgraded, then closed by the overload path. Found by the 1.0 benchmark: 5000 idle connections lost 905, and a 5000-subscriber broadcast delivered to nobody because the publisher's own connection was one of the dropped ones. Nothing in the configuration, the metrics or the documentation said so.

**Decision.** `EmbeddedConfig::queue()` derives the two limits the operator did not set: events = `max_connections` + 4096, bytes = `max_connections` * `kReservedCloseBytes` + 8 MiB. One slot per connection, plus the old defaults as room for events in flight. A limit the operator did set is returned exactly as set (an explicit tiny queue is how the overload path is tested, and a bounded queue is still the point), and the documentation says that a queue below `max_connections` caps the server below it.

**Rejected.** (a) Raising the constant to a bigger fixed number: moves the cliff, keeps the inconsistency. (b) Refusing a start when an explicit queue is smaller than `max_connections`: breaks the tests and embedders that choose a small queue on purpose. (c) Releasing the Close reservation: the whole point of it.

**Consequences.** + The default configuration is consistent: every connection the server accepts has room. + Memory stays bounded: a reservation is a counter, not an allocation, and the payload bytes waiting are still capped by `event_queue_bytes`. - An operator who sets `event_queue_events` below `max_connections` can still cap connections silently; the OPERATIONS table says so.

### A3 (2026-11-24): the event descriptor may lag the queue by one loop iteration (ADR-0035)

**Problem.** Section 3 said the descriptor is readable exactly while the queue is non-empty. ADR-0035 batches the host's wake-up: a Text or Binary event pushed from a loop thread into an empty queue owes the wake-up until that loop's flush at the end of its iteration (a context switch per message on one CPU was the cost of paying it at once).

**Decision.** The contract becomes: the descriptor is readable no later than the end of the loop iteration that queued an event; `wsrv_poll()` / `try_pop()` return the event as soon as it is queued. Nothing is lost: the owing loop always flushes (every message comes from a callback inside an iteration, and the hook is the last step of it), a host that empties the queue first cancels the owed wake-up, and Open, Close, Error and Handshake events still write at once (when they find the queue empty). An event pushed with `Wake::Now` onto a non-empty queue relies on the wake-up written or owed already (ADR-0035). `include/wsrv/wsrv.h`, `docs/BINDINGS.md` and the daemon's comments say so; a host must not use the descriptor's readability as proof that the queue is empty. The ABI does not change (no symbol, no version).

**Consequences.** + Fewer wake-ups and context switches under load. - A host that waits on the descriptor alone sees a message up to one loop iteration (bounded by the loop's fairness budgets, in practice microseconds) after `wsrv_poll()` could have returned it. ADR-0013 and ADR-0014 (the loop's task queue and fairness budgets) are unchanged: the hook runs after what they schedule.
