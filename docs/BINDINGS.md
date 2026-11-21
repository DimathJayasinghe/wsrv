# Language Bindings & Embedding Strategy

Goal: PHP (and any other language) gets WebSocket server power from one audited C++ core. PHP cannot run a long-lived, event-driven socket server inside classic request/response models (PHP-FPM, mod_php); this library provides that capability.

## 1. Core idea

```
 ┌────────────── host process (PHP / Node / Python / Go / …) ──────────────┐
 │  user code (language thread)                                            │
 │     ▲  events (message/open/close)          │ commands (send/close/sub) │
 │     │                                       ▼                           │
 │  binding layer (thin, per language)  ── calls ──►  libwsrv C ABI        │
 │                                                  ┌───────────────────┐  │
 │                                                  │ wsrv I/O thread(s)│  │
 │                                                  │ (C++ core, epoll) │  │
 │                                                  └───────────────────┘  │
 └──────────────────────────────────────────────────────────────────────────┘
```

Rule: **the language runtime never runs on a wsrv I/O thread, and wsrv I/O threads never call into the language runtime.** They exchange messages through bounded queues. This avoids GIL/Zend thread-safety problems, deadlocks, and re-entrancy bugs.

## 2. Integration modes (all share one core)

| Mode | How | Best for | Trade-offs |
|---|---|---|---|
| **A. Embedded (in-process)** | Binding loads `libwsrv`, starts I/O thread(s) inside the host process. Host drains an event queue and enqueues commands. | PHP CLI workers (long-running scripts), Node, Python, Go | Lowest latency; host crash takes server down; needs native ext/FFI per language |
| **B. Sidecar (out-of-process)** | `wsrvd` daemon owns sockets. Apps talk to it over a local IPC protocol (Unix domain socket; length-prefixed binary/JSON frames) or HTTP/gRPC webhook-style callbacks. | **PHP-FPM / Laravel / Symfony** classic stacks, polyglot systems | One extra hop; deployment of a second process; but isolates crashes and works with any language that can open a socket |
| **C. Native C++ library** | Link `wsrv::wsrv` directly | C++ users | — |

Recommendation: ship **A + B**, with B implemented first for broadest reach (PHP-FPM works with no extension), then A for performance. Mode B's IPC client libraries are tiny and can be written in any language.

## 3. Stable C ABI (`include/wsrv/wsrv.h`)

- Pure C99 header, opaque handles, no C++ types, no exceptions across the boundary, no allocation ownership ambiguity.
- Versioned: `wsrv_abi_version()`; additive changes only within a major version; symbol visibility hidden except `WSRV_API` functions.
- Thread-safety documented per function (`any-thread` vs `owner-thread`).

Sketch:

```c
The v0 surface as built (ADR-0016 slice 3; the header `include/wsrv/wsrv.h` is the authority and this
sketch is kept in step with it). Not yet built, and still planned for later ADRs, are the lines marked `(planned)`.

```c
typedef struct wsrv_server wsrv_server;
typedef struct wsrv_config wsrv_config;
typedef uint64_t wsrv_conn_id;            /* process-unique, never reused; 0 is never a connection */

typedef struct wsrv_error { wsrv_status status; char message[128]; } wsrv_error;  /* caller-owned */

typedef struct wsrv_event {
  uint32_t        struct_size;            /* caller sets sizeof(wsrv_event) before wsrv_poll */
  wsrv_event_type type;                   /* OPEN, TEXT, BINARY, CLOSE, ERROR */
  wsrv_conn_id    conn;
  const uint8_t  *data;  size_t len;      /* valid until wsrv_event_release(); never NULL */
  uint16_t        close_code;  int clean;
  void           *reserved;
} wsrv_event;

wsrv_config *wsrv_config_new(void);
wsrv_status  wsrv_config_set(wsrv_config*, const char *key, const char *val, wsrv_error*);  /* validated */
wsrv_status  wsrv_server_create(const wsrv_config*, wsrv_server **out, wsrv_error *err);
wsrv_status  wsrv_server_start(wsrv_server*, wsrv_error*);    /* spawns the I/O thread, waits until listening */
uint16_t     wsrv_server_port(const wsrv_server*);
int          wsrv_server_event_fd(const wsrv_server*);        /* readable when events pending (eventfd) */
wsrv_status  wsrv_poll(wsrv_server*, wsrv_event *out, uint32_t timeout_ms); /* host thread drains */
void         wsrv_event_release(wsrv_event*);
wsrv_status  wsrv_send_text(wsrv_server*, wsrv_conn_id, const char*, size_t);   /* any-thread, non-blocking */
wsrv_status  wsrv_send_binary(wsrv_server*, wsrv_conn_id, const uint8_t*, size_t);
/* (planned) the rest of this block is the design sketch for later ADRs: */
/* streaming send / receive of fragmented messages (see FRAGMENTATION.md):
   events WSRV_EV_TEXT_PART / WSRV_EV_BINARY_PART carry an is_final flag */
wsrv_status  wsrv_begin_message(wsrv_server*, wsrv_conn_id, int is_text);     /* WSRV_ERR_MESSAGE_IN_PROGRESS if open */
wsrv_status  wsrv_write_part(wsrv_server*, wsrv_conn_id, const uint8_t*, size_t); /* WSRV_ERR_BACKPRESSURE → wait */
wsrv_status  wsrv_end_message(wsrv_server*, wsrv_conn_id);
wsrv_status  wsrv_close(wsrv_server*, wsrv_conn_id, uint16_t code, const char *reason, size_t reason_len);  /* built */
wsrv_status  wsrv_publish(wsrv_server*, const char *topic, const uint8_t*, size_t, int is_text);  /* (planned, M10) */
wsrv_status  wsrv_subscribe(wsrv_server*, wsrv_conn_id, const char *topic);                       /* (planned, M10) */
wsrv_status  wsrv_server_stop(wsrv_server*);   /* built; grace = config key shutdown_grace_ms (ADR-0016) */
void         wsrv_server_destroy(wsrv_server*);  /* built */
```

Key design points:
- **Pull model** (`wsrv_poll` + `event_fd`) so any language can integrate with its own loop (ReactPHP/Revolt/Swoole-free setups, libuv, asyncio, Go netpoller) with no callbacks into the runtime. Optional push-callback variant is allowed only for languages with safe thread re-entry (Go via cgo, Rust), documented per binding.
- **Connection IDs, not pointers**: no dangling handles; stale ID ⇒ `WSRV_ERR_NO_SUCH_CONN`.
- **Bounded queues**: event queue full ⇒ pause reads (backpressure) rather than drop silently; policy configurable.
- **Handshake authentication hook**: host-side decision requires round trip; implemented as async: server emits `WSRV_EV_HANDSHAKE` (headers snapshot) and waits (bounded timeout) for `wsrv_handshake_accept/reject`.
- **Memory**: all event payloads owned by the core until released; bindings copy into runtime-native strings.
- ABI tested with a C-only consumer and `abi-compliance-checker` in CI.

## 4. PHP specifics

### 4.1 Mode A options
1. **PHP extension (`ext/wsrv`, C++ over C ABI)** — recommended for performance. Registers a `Wsrv\Server` class; background thread is wsrv's I/O thread, **not** a PHP thread. PHP code runs `$server->poll($timeoutMs)` or registers the `event_fd` stream with ReactPHP/Revolt/`stream_select`. Requires CLI/long-running SAPI; refuses to start under FPM (clear error).
2. **PHP FFI binding (`ext-ffi`)** — zero-compile install (Composer package ships the `.so` + header), slightly slower, easiest adoption; good default for v1 of the PHP binding.
3. ❌ Running PHP userland on multiple threads (`pthreads`/`parallel` inside I/O path) — rejected: ZTS fragility, shared-nothing copying, no benefit since I/O is already off-PHP.

### 4.2 Mode B (sidecar) for PHP-FPM
```
Browser ⇄ wsrvd (WebSocket) ⇄ Unix socket IPC ⇄ PHP worker(s) (CLI daemon using the Composer client)
Laravel/Symfony request (FPM) ──► `Wsrv\Client::publish()` ──► wsrvd ──► clients
```
FPM requests only *publish* (short-lived connection / persistent pconnect to wsrvd); inbound client messages are delivered to a CLI worker pool or HTTP webhook with HMAC signature. Delivery semantics: at-most-once by default; optional at-least-once with ack + replay window (documented).

**What exists today (M12 slice 1, `bindings/php/`, pure PHP, no extension).** Two classes over the IPC protocol (`docs/IPC_PROTOCOL.md`):
- `Wsrv\Client`: one connection to `wsrvd`: `connect()`, `poll()`, `sendText()`, `sendBinary()`, `closeConnection()`, `ping()`.
- `Wsrv\Worker`: the callback style on top of it: `on('open'|'message'|'close'|'error'|'status', ...)`, `run()`, `step()`, `stop()`, with a `Connection` handle given to callbacks.

`Wsrv\Client::publish()` / `subscribe()` (publish-only FPM requests, topics and rooms) are **not** implemented: the C++ `Broker` exists (M10, ADR-0024) but exposing it to a host language needs its own design (ADR-0024 section 7). The *embedded* mode (FFI, Mode A) exists as `Wsrv\Embedded\Server` (M12 slice 2, ADR-0025): lifecycle, `open`/`message`/`close`/`error` handlers, `send`/`close`, `step()`/`run()`. Still open for it: the handshake hook, topics and rooms, the loop adapters. The native PHP extension is not planned for 1.0. The class below is the target ergonomics (the final name is `Wsrv\Embedded\Server`).

### 4.3 PHP API (target ergonomics)

```php
$server = new Wsrv\Server(
    listen: '0.0.0.0:8080',
    allowedOrigins: ['https://app.example.com'],
);
$server->on('open',    fn(Wsrv\Connection $c) => $c->subscribe('lobby'));
$server->on('message', fn(Wsrv\Connection $c, string $msg) => $server->publish('lobby', $msg));
$server->on('close',   fn(Wsrv\Connection $c, int $code) => null);
$server->run();                      // blocking loop on the PHP thread: poll → dispatch callbacks
```
Framework adapters (Laravel, Symfony) and loop adapters (ReactPHP, Revolt/Amp) as separate Composer packages.

## 5. Other languages

| Language | Mechanism | Notes |
|---|---|---|
| C / C++ | C ABI / native API | reference |
| Python | `cffi` or `pybind11` module; asyncio integration via `event_fd` + `loop.add_reader` | wheels via cibuildwheel |
| Node.js | N-API addon (`node-addon-api`), `event_fd` integrated with libuv (`uv_poll`) | prebuilds via prebuildify |
| Go | cgo wrapper; goroutine drains `wsrv_poll` | or pure-Go IPC client for sidecar |
| Rust | `wsrv-sys` (bindgen) + safe `wsrv` crate | |
| Java/Kotlin | Panama FFI (JDK 22+) | |
| .NET | P/Invoke | |
| Ruby | FFI gem | |
| Any | Sidecar IPC protocol (documented, versioned) | minimal client ≈ 200 lines |

All bindings are thin: **no protocol logic outside the C++ core.** Conformance for each binding = run the same Autobahn suite through a binding-based echo server in CI.

## 6. Platform (OS/arch) support roadmap

| Platform | Priority | Mechanism |
|---|---|---|
| Linux x86-64/aarch64 | v1 | epoll |
| macOS | v1.x | `KqueuePoller` (IPoller) |
| Windows | v1.x–2 | IOCP or `wepoll`-style `IPoller`; Winsock `Socket` adapter |
| FreeBSD | later | kqueue |
| musl/Alpine | v1 (CI) | must build and pass |

Distribution: prebuilt binaries per platform (GitHub Releases), Composer/pip/npm/crates packages bundling or downloading verified artifacts (checksums + signatures).

## 7. Risks specific to bindings

| Risk | Mitigation |
|---|---|
| ABI break | Versioned C ABI, ABI checker in CI, additive-only policy |
| Event queue overload when host is slow | Bounded queue + read-pause backpressure; metrics |
| Host runtime fork() (PHP-FPM pre-fork, `pcntl_fork`) with live threads | Document: create server **after** fork; detect via pid check and error |
| Signal handling clash with host runtime | Library installs **no** signal handlers; host owns signals; `wsrv_server_stop()` is explicit |
| Memory ownership bugs across FFI | Copy-on-receive; explicit release; ASAN builds of bindings in CI |
| Maintenance cost of many bindings | Binding tier policy (Tier 1 maintained by core team; Tier 2 community) in GOVERNANCE.md |
| Security of sidecar IPC | Unix socket perms, optional peer-cred check, HMAC on HTTP webhooks, max frame size on IPC |
