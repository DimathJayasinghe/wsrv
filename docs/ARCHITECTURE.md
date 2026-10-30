# Architecture

## 1. Principles

1. **Sans-I/O core.** Protocol code consumes bytes and produces events + bytes. It never touches sockets, threads, clocks, or logging backends directly (those are injected).
2. **Shared-nothing concurrency.** A connection lives on exactly one event loop for its whole life. No locks on connection state.
3. **Dependencies point inward.** Policy (protocol) does not depend on mechanism (epoll, TLS).
4. **Bounded everything.** Every buffer, queue, and table has a limit and a defined overflow behaviour.
5. **Errors are values.** `Result<T, Error>` on runtime paths; no exceptions crossing module boundaries.
6. **Small public surface.** Users see `include/wsrv/` only; everything else is private.

## 2. Layered view

```
┌───────────────────────────────────────────────────────────────┐
│ Application  (user code: IWebSocketHandler, examples)         │
├───────────────────────────────────────────────────────────────┤
│ server/   Server facade · ServerBuilder · Connection ·        │
│           ConnectionRegistry · Broker (pub/sub) · Limits      │
├──────────────────────────────┬────────────────────────────────┤
│ protocol/  (sans-I/O)        │ net/  (OS, mechanism)          │
│  HandshakeNegotiator         │  Socket (RAII) · IPoller/Epoll │
│  FrameParser/FrameWriter     │  EventLoop · TimerWheel        │
│  MessageAssembler            │  Acceptor · ITransport         │
│  Utf8Validator               │   ├ PlainTransport             │
│  ProtocolSession (FSM)       │   └ TlsTransport (decorator)   │
│  extensions/ (deflate)       │  WakeupChannel (eventfd)       │
├──────────────────────────────┴────────────────────────────────┤
│ http/   RequestParser · ResponseWriter                        │
├───────────────────────────────────────────────────────────────┤
│ crypto/ Sha1 · Base64               base/  Result · Buffers · │
│                                     Clock · IRandom · ILogger │
│                                     · IMetrics                │
└───────────────────────────────────────────────────────────────┘
```

### 2.1 Allowed dependencies (enforced by `tools/check_includes.py` on every `#include`; the declared link graph is diffed by the `module_graph` test as a second guard)

| Module | May depend on |
|---|---|
| base | std only |
| crypto | base |
| http | base |
| protocol | base, crypto, http |
| extensions | base, protocol (interfaces only) |
| net | base |
| server | base, http, protocol, net, extensions |
| capi (`include/wsrv/wsrv.h`, `src/capi/`) | server (wraps it; catches all exceptions; exports C symbols only) |
| wsrvd (sidecar daemon) | capi/server, IPC codec |
| bindings/* | capi only (never C++ internals) |
| include/wsrv (public C++) | forward-declares; implemented by server |

Forbidden: `protocol → net`, `protocol → <sys/*>`, `net → protocol`, any cycle, any module → `server`.

## 3. Components

### 3.1 base
- `Result<T,E>` = alias of `std::expected`; `Error` = `{ErrorCode code; std::string_view context}` (no allocation on hot path).
- `ByteBuffer` (growable, bounded, contiguous), `RingBuffer` (fixed capacity), `BufferPool` (slab, thread-local per loop), `ByteView = std::span<const std::byte>`.
- `IClock` (steady + system), `IRandom` (`fill(span<byte>)`), `ILogger`, `IMetrics` (injected; no-op defaults).

### 3.2 crypto
- `Sha1` (streaming), `Base64::encode/decode` (strict). Pure computation only: no OS calls. Randomness is *not* here: see `IRandom` in base and `SystemRandom` in net (ADR-0012).
- Reviewed against RFC test vectors; not for general-purpose security use (documented).

### 3.3 http
- `RequestParser`: incremental state machine, strict RFC 9112 subset (request-line, headers; rejects obs-fold, bare LF, NUL, duplicate singleton headers, oversize). Output: `HandshakeRequest` view (string_views into a stable buffer) — zero copy.
- `ResponseWriter`: serialises 101/4xx/5xx; header values sanitised (no CR/LF).

### 3.4 protocol (sans-I/O)
| Class | Responsibility (single) |
|---|---|
| `HandshakeNegotiator` | Run `IHandshakeRule` chain, produce `HandshakeDecision` (accept + selected subprotocol/extensions, or reject + status) |
| `FrameParser` | Incremental bytes → `FrameHeader` + payload chunks; validates header rules |
| `FrameWriter` | Build frame headers/payload for outbound messages |
| `Masker` | XOR masking (scalar → SIMD later), alignment-safe |
| `Utf8Validator` | Incremental DFA-based validator, state survives fragment boundaries |
| `MessageAssembler` | Fragment sequencing, size/fragment limits, incomplete-message timer, UTF-8 for text; buffered or streaming delivery ([FRAGMENTATION.md](FRAGMENTATION.md)) |
| `OutboundScheduler` | Auto-fragmentation of outbound messages; control-frame priority at frame boundaries; one open data message at a time |
| `MessageWriter` | Streaming send handle with backpressure; closes 1011 if dropped unfinished |
| `CloseCode` | Legality table and helpers |
| `ProtocolSession` | FSM: `Open → Closing → Closed` (constructed after the 101 response); orchestrates parser/assembler/scheduler/ping/close; **input** `next_event(input)`, `tick(now)`, `send_*()`, `close()`, `on_transport_closed()`; **output** `pending_output()` / `consume_output(n)` (surface below) |
| `IExtension` | Frame transform hook (RSV bits, per-message transform) |

`ProtocolSession` surface (as implemented in M4, `src/protocol/session.hpp`):

```cpp
class ProtocolSession {
public:
  ProtocolSession(SessionConfig cfg, TimePoint now);        // starts Open: the 101 is already sent
  std::optional<SessionEvent> next_event(MutableByteView& input); // consumes input; Message/Part, Ping, Pong, ProtocolError, Closed
  void tick(TimePoint now);                                  // timers (idle, heartbeat, incomplete message, close) + rate windows
  Result<void> send_text(std::string_view);                  // Closing | InvalidUtf8 | Backpressure | BufferFull
  Result<void> send_binary(ByteView);
  Result<void> send_ping(ByteView);
  Result<void> close(CloseCode, std::string_view reason);    // idempotent
  void on_transport_closed();                                // EOF / reset: Closed at once
  ByteView pending_output();                                 // bytes to write (control frames first at frame boundaries)
  void consume_output(std::size_t n);
  SessionState state() const noexcept;                       // Open -> Closing -> Closed
};
```
Why `next_event(input)` and not `feed()` + a queue: events carry views into the caller's buffer (frames are unmasked in place) and into the assembler's message buffer; a queue would outlive them. Pulling one event at a time keeps every view valid until the next call. Server role only for now (inbound frames must be masked, outbound are not); a client role needs `IRandom` for mask keys.

The session is deterministic: same bytes + same ticks ⇒ same output. This is what makes fuzzing and golden tests cheap.

### 3.5 net
As built in M5. Every kernel call goes through `ISyscalls`, so each errno path is testable (`FakeSyscalls`).
| Class | Responsibility |
|---|---|
| `SystemRandom` | `IRandom` adapter over `getrandom(2)` (the only CSPRNG source; ADR-0012) |
| `Fd` | RAII ownership of a descriptor; move-only, closes once |
| `map_errno`, `retrying` | `errno` to `Error` (`WouldBlock`, `ConnectionClosed`, `FdLimit`, `AddressInUse`, `Io`); bounded `EINTR` retry |
| `ISyscalls` / `LinuxSyscalls` | One interface for socket, accept4, read, send (`MSG_NOSIGNAL`), epoll, eventfd, open_null, close |
| `IPoller` / `EpollPoller` | Register/modify/wait; edge-triggered; tokens, never fd numbers, identify registrations |
| `TimerWheel` | Hashed wheel (10 ms tick), O(1) arm/cancel, fire budget, monotonic deadlines only |
| `EventLoop` | Poller + wheel + bounded task queue (mutex-protected deque) + wake-up eventfd; `run()`, `run_once()`, `stop()`, `post(fn)`, `run_after(d, fn)`, `watch/rewatch/unwatch` (generation-checked `WatchHandle`); per-iteration budgets for events, timers and tasks |
| `Endpoint` | Parsed IPv4/IPv6 literal + port (no DNS) |
| `TcpListener` | Non-blocking listening socket (`SO_REUSEADDR`, optional `SO_REUSEPORT`) |
| `Acceptor` | Accept burst with a budget; `EMFILE` reserve-fd shedding, back-off timer, `shed_count` |
| `ITransport` / `PlainTransport` | `read` (0 = EOF, `WouldBlock`), `write` (partial, `ConnectionClosed`), `shutdown_write`, `close`; TLS will decorate it (M8) |

Thread-safe surface of a loop: `post()` and `stop()` only. Everything else runs on the loop thread.

### 3.6 server
| Class | Responsibility |
|---|---|
| `Server` (Facade) | Start/stop; owns `EventLoopGroup`, acceptors, registry |
| `ServerBuilder` (Builder) | Validated, immutable `ServerConfig`; the only place exceptions are allowed (invalid config) |
| `EventLoopGroup` | N loops (+threads); assigns new connections (round-robin or `SO_REUSEPORT`) |
| `Connection` | Glue for one client: `ITransport` + `ProtocolSession` + write queue + timers; implements loop callbacks |
| `ConnectionRegistry` | Per-loop table (no cross-loop lock); global counters via atomics |
| `IWebSocketHandler` | User callbacks (Observer) |
| `Session` / `SessionHandle` | In-loop API vs cross-thread, weak, loop-posted API |
| `Broker` | Topic → subscribers (per-loop shards; cross-loop fan-out via `post`) |
| `LimitsEnforcer` | Per-IP and global caps, rate limiters |

**As built in M6** (single loop; ADR-0013, lesson 11). Implemented: `Server`, `ServerBuilder`, `Connection`, `IConnectionHost` + `LoopHost` (Connection's only view of the loop), `ConnectionTable` (plays the registry's role; erases retired connections at `EventLoop::defer`), `ServerConfig`, `IWebSocketHandler`/`Session`/`IHandlerFactory`; `HandshakeDriver` lives in `protocol/` (pure bytes to outcome). Not yet built, by milestone: `Broker` (M7+), `LimitsEnforcer` (M10).

**As built in M7** (ADR-0014): `ServerConfig::threads` loops. `LoopShard` owns everything per loop (loop, host, table, scratch, context, handle link); `WorkerThread` runs shards 1..N-1 on threads the server starts inside `run()`; loop 0 runs on the caller's thread, owns the listener and acceptor, and hands each new connection to the next loop by `post`. `EventLoopGroup` is therefore `Server` plus its shards. The connection cap and ids are global (atomic); `SessionHandle` (id plus a guarded `LoopLink`) sends from any thread. Streaming send (ADR-0015, lesson 12): `Session::begin_text/begin_binary` return a loop-thread-only `MessageWriter` bound to its connection through a clearable `StreamLink`; `Backpressure` is a signal, and `IWebSocketHandler::on_writable` follows on a later loop iteration once the queue is at half its limit.

**API location:** until 1.0-rc the public API headers stay in `src/server/` (`handler.hpp`, `server_builder.hpp`, `config.hpp`); they move to `include/wsrv/` once the C ABI (M11) fixes what is public. See ADR-0013.

## 4. Public API (sketch, `include/wsrv/`)

```cpp
namespace wsrv {

struct CloseInfo { CloseCode code; std::string reason; bool clean; };

class IWebSocketHandler {
public:
  virtual ~IWebSocketHandler() = default;
  virtual void on_open(Session&)                                   {}
  virtual void on_text(Session&, std::string_view)                 {}
  virtual void on_binary(Session&, std::span<const std::byte>)     {}
  virtual void on_message_part(Session&, MessagePart, bool final)  {}  // streaming delivery mode only
  virtual void on_close(Session&, const CloseInfo&)                {}
  virtual void on_error(Session&, const Error&)                    {}
};

class Session {            // valid only on its loop thread, only inside callbacks
public:
  Result<void> send_text(std::string_view);
  Result<void> send_binary(std::span<const std::byte>);   // auto-fragmented if large
  Result<MessageWriter> begin_text();                     // streaming send, see FRAGMENTATION.md
  Result<MessageWriter> begin_binary();
  void close(CloseCode = CloseCode::Normal, std::string_view reason = {});
  SessionHandle handle();  // copyable, thread-safe
  const PeerInfo& peer() const;
};

class SessionHandle {      // weak; send/close are posted to the owning loop
public:
  bool send_text(std::string_view);   // false if session gone / queue full
  void close(CloseCode, std::string_view);
};

class ServerBuilder { /* fluent: listen(), limits(), tls(), origins(), handler_factory(), threads(), logger(), metrics() */ };
std::unique_ptr<Server> ServerBuilder::build();   // throws ConfigError only here
}
```

Rules: callbacks never block; handlers must not retain `Session&` past the callback (use `SessionHandle`); `string_view`/`span` arguments are valid only for the call duration.

## 5. Threading model

```
            ┌──────────────┐ accept        ┌────────────┐
  clients ─►│  Acceptor(s) │──────────────►│ EventLoop 0│─ Connection A,B,C
            └──────┬───────┘  assign fd    ├────────────┤
                   └─────────────────────►│ EventLoop 1│─ Connection D,E
                      (round-robin or      ├────────────┤
                       SO_REUSEPORT)       │ EventLoop N│─ ...
                                           └────────────┘
 other threads ──SessionHandle::send──► loop.post(task) ──(eventfd wake)──► loop thread
```

- Loop thread owns: its connections, timers, buffer pool, registry shard.
- Cross-thread communication = MPSC queue + eventfd. No shared mutable connection state.
- **As built (M7, ADR-0014, lesson 13):** `threads` loops, each a `LoopShard`; loop 0 runs on the `Server::run()` caller and owns the acceptor, which hands descriptors to the next loop by `post`. The only mutexes are the task queue inside `EventLoop` and the `LoopLink` behind `SessionHandle`, each held for one bounded queue operation; `Connection` and `ProtocolSession` contain none. The task queue is bounded (`BufferFull`, EC-N14). Shutdown is posted to every shard and ends when all report drained or the grace timer forces it.
- Global state is limited to atomics (counters) and immutable config (`shared_ptr<const ServerConfig>`).
- Hot reload (certs, limits) = swap a new immutable snapshot via `atomic<shared_ptr>`; connections pick it up on next use.

## 6. Data flow

**Inbound:** `epoll_wait` → `Connection::on_readable` → loop `read()` until `EAGAIN` or fairness budget (e.g. 16 reads/64 KiB) → `ProtocolSession::feed` → events → `IWebSocketHandler`. If handler/outbound is saturated → disable read interest (backpressure).

**Outbound:** `Session::send_*` → `ProtocolSession` frames into output buffer → `Connection::flush` tries `write` immediately → remainder queued; arm `EPOLLOUT` only while queue non-empty → bounded by `max_outbound_bytes`.

**Handshake:** accept → plain/TLS transport → `RequestParser` (timer: handshake timeout) → `HandshakeNegotiator` (rule chain) → write 101 → construct `ProtocolSession`, feed leftover bytes.

**Close:** either side → `Closing` → send Close → wait for peer Close or deadline → `shutdown(SHUT_WR)` → close fd → `on_close` exactly once → removal from registry.

## 7. Connection lifecycle (FSM)

```
Accepted ─► TlsHandshaking? ─► HttpHandshaking ─► Open ─► Closing ─► Closed
    │              │                 │             │          │
    └──────────────┴────── error / timeout ────────┴──────────┘ (any state → Closed)
```
Invariants: `on_open` at most once and only from `Open`; `on_close` exactly once if `on_open` ran; `Closed` is terminal and releases all resources deterministically (RAII).

## 8. Error model

- `ErrorCode` enum with category (`Protocol`, `Limit`, `Io`, `Tls`, `Config`, `Internal`).
- Protocol/Limit errors map to a close code (table in `protocol/close_code.hpp`); Io errors map to 1006 (abnormal, never sent).
- Programmer errors → `WSRV_ASSERT`, active in release builds too, which prints the failed condition and calls `std::abort()` for invariant violations that would imply memory unsafety. No silent recovery from broken invariants.

## 9. Extensibility points (Open/Closed)

| Need | Extension point | Existing impls |
|---|---|---|
| New OS poller (kqueue/io_uring/IOCP) | `IPoller` | `EpollPoller` |
| New transport (TLS, Unix socket, PROXY protocol) | `ITransport` / decorator | `PlainTransport`, `TlsTransport` |
| New handshake policy (auth, rate limit) | `IHandshakeRule` | origin, subprotocol, auth hook |
| New WS extension | `IExtension` | permessage-deflate |
| Logging/metrics backend | `ILogger`, `IMetrics` | stderr JSON, no-op |
| Scheduling/assignment | `ILoopAssigner` | round-robin, least-loaded |

## 10. Memory strategy

- Per-loop `BufferPool` of fixed size classes (4 KiB, 16 KiB, 64 KiB, …); connections borrow/return; idle connections hold ≤ 1 small buffer.
- Outbound messages for broadcast are framed once and shared (`shared_ptr<const Frame>` ref-counted across loops) — avoids N copies.
- No `new`/`delete` in hot path; `std::pmr` allowed with pool resources.

## 11. Observability hooks

Logger, metrics, and a lightweight `ITraceSink` are injected via builder. Every connection has a 64-bit ID. Log fields: `ts level conn_id peer event code msg`. Log sanitiser strips control chars from peer-supplied strings.

## 12. Decision records

Structural changes require an ADR in [adr/](adr/). Current: 0001–0008.
