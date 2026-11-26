# Requirements

Keywords MUST / SHOULD / MAY follow RFC 2119. IDs are stable; tests and PRs cite them.

## 1. Standards in scope

| Standard | Scope |
|---|---|
| RFC 6455 | The WebSocket Protocol — full server side (MUST) |
| RFC 7230/9112 (subset) | HTTP/1.1 request parsing for the Upgrade handshake only |
| RFC 3629 | UTF-8 validation of text frames and close reasons |
| RFC 3174 / RFC 4648 | SHA-1 and Base64 used by `Sec-WebSocket-Accept` |
| RFC 7692 | permessage-deflate (SHOULD, M9) |
| RFC 8446 / 5246 | TLS 1.3 / 1.2 via OpenSSL (SHOULD, M8) |
| RFC 6265 / 6454 | Cookie and Origin semantics for handshake policy |

## 2. Functional requirements

### 2.1 Handshake
- **FR-H1** MUST accept only `GET` with HTTP/1.1+, valid `Host`, `Upgrade: websocket` (case-insensitive), `Connection` containing `upgrade` token, `Sec-WebSocket-Key` (base64 of exactly 16 bytes), `Sec-WebSocket-Version: 13`.
- **FR-H2** MUST respond `426 Upgrade Required` with `Sec-WebSocket-Version: 13` for other versions.
- **FR-H3** MUST compute `Sec-WebSocket-Accept` = base64(SHA-1(key + GUID)).
- **FR-H4** MUST support subprotocol negotiation via application-supplied preference list; reject (no header) if none match, per policy.
- **FR-H5** MUST enforce configurable Origin policy (default: deny when Origin present and not allow-listed).
- **FR-H6** MUST support application handshake hooks (authN/Z) as an ordered chain that can accept, reject (with status), or annotate the connection.
- **FR-H7** MUST bound handshake size, header count, line length, and time (see §4).
- **FR-H8** MUST preserve any bytes following the handshake in the same read as the first frame bytes.
- **FR-H9** MUST reject malformed requests with an appropriate 4xx and close, never crash or leak.

### 2.2 Framing & messages
- **FR-F1** MUST parse frames incrementally from arbitrary byte-chunk boundaries (1 byte at a time must work).
- **FR-F2** MUST require client frames masked; MUST NOT mask server frames.
- **FR-F3** MUST validate RSV bits (0 unless an extension negotiated them), opcodes, minimal length encoding, 64-bit length MSB = 0, control-frame rules (≤125 bytes, FIN=1).
- **FR-F4** MUST reassemble fragmented messages with correct interleaved-control-frame handling.
- **FR-F5** MUST validate text messages as UTF-8, including across fragment boundaries, and fail fast (1007) at the first invalid byte.
- **FR-F6** MUST enforce max frame size and max message size (1009).
- **FR-F7** MUST support sending text, binary, fragmented (streaming) messages, ping, pong, close.
- **FR-F8** MUST offer two delivery modes: buffered (complete message, default) and streaming (parts with `final` flag, text parts ending on UTF-8 code-point boundaries). See [FRAGMENTATION.md](FRAGMENTATION.md).
- **FR-F9** MUST auto-fragment outbound messages larger than `max_outbound_frame`, transparently to the peer.
- **FR-F10** MUST send queued control frames (Ping/Pong/Close) at the next frame boundary, ahead of remaining data fragments, and never interleave two outbound data messages.
- **FR-F11** MUST time out incomplete fragmented messages (`incomplete_message_timeout`) and count partial-message bytes against memory budgets.
- **FR-F12** MUST provide a non-blocking streaming writer (`MessageWriter`) with backpressure signalling.

### 2.3 Control & close
- **FR-C1** MUST answer Ping with Pong (same payload) promptly, even mid-fragmentation.
- **FR-C2** MUST perform the closing handshake; echo close code; time out waiting for peer close.
- **FR-C3** MUST validate close payload (0 or ≥2 bytes; legal code set; UTF-8 reason) and respond 1002 on violation.
- **FR-C4** MUST NOT send codes 1005/1006/1015 on the wire.
- **FR-C5** MUST ignore all data after Close is received, and stop reading after close completes.
- **FR-C6** SHOULD provide server-initiated heartbeat (ping interval + pong timeout) and idle timeout.

### 2.4 Server behaviour
- **FR-S1** MUST handle ≥ configured max concurrent connections; refuse further with minimal cost.
- **FR-S2** MUST provide a handler interface: `on_open`, `on_message`, `on_ping/pong` (optional), `on_close`, `on_error`.
- **FR-S3** MUST provide thread-safe `SessionHandle` for sending from other threads.
- **FR-S4** MUST implement backpressure: bounded per-connection outbound queue with configurable slow-consumer policy; inbound read pause when handler lags.
- **FR-S5** MUST support graceful shutdown (stop accepting, send 1001, drain with deadline, force close).
- **FR-S6** MUST isolate user-handler failures: exceptions caught at the boundary → close 1011, logged, server continues.
- **FR-S7** SHOULD provide pub/sub topics and broadcast with per-subscriber backpressure isolation (M10). Done for C++ applications (`Broker`, ADR-0024); the C ABI and PHP exposure are a follow-up.
- **FR-S8** MAY expose `/healthz` (plain HTTP 200) on the same port.

### 2.5 Extensions & transport
- **FR-E1** SHOULD support permessage-deflate negotiation (window bits, no-context-takeover) with size/ratio limits (M9).
- **FR-T1** SHOULD support TLS (`wss://`) with hardened defaults, SNI, certificate hot-reload (M8).
- **FR-T2** MUST keep TLS invisible to protocol layers (transport decorator).

## 3. Non-functional requirements

### 3.1 Performance (targets; verified by `tests/bench`, regressions > 5 % fail nightly)
| ID | Target |
|---|---|
| NFR-P1 | 100 000 idle connections on 1 node with ≤ 8 KiB average per-connection memory (plain) |
| NFR-P2 | Echo of 128-byte messages: ≥ 500k msg/s on 4 cores (loopback, plain) |
| NFR-P3 | p99 echo latency < 1 ms at 50 % CPU (loopback) |
| NFR-P4 | Zero heap allocation per message on the steady-state echo path (pooled buffers) |
| NFR-P5 | Handshake throughput ≥ 20k/s/core |

### 3.2 Reliability
- **NFR-R1** No crash, hang, or leak on any input (fuzz-proven). 
- **NFR-R2** Survive fd exhaustion (`EMFILE`/`ENFILE`), `ENOMEM`, `EINTR`, `EPIPE`, `ECONNRESET`, partial reads/writes.
- **NFR-R3** 24 h soak with churn: flat RSS (±2 %), no fd leak.

### 3.3 Security
See [SECURITY_DESIGN.md](SECURITY_DESIGN.md). Secure by default; unsafe options require explicit, loudly named opt-in (e.g. `allow_any_origin`).

### 3.4 Maintainability
- **NFR-M1** Public API stable and documented (Doxygen comments on all public symbols).
- **NFR-M2** Module dependency rules enforced in CMake (see ARCHITECTURE §3).
- **NFR-M3** Cyclomatic complexity ≤ 10 per function (target), file ≤ 500 lines (guideline).
- **NFR-M4** Coverage: `protocol/`, `http/`, `base/`, `crypto/` ≥ 90 % line / 85 % branch; others ≥ 75 %.

### 3.5 Portability
- **NFR-X1** Linux x86-64 and aarch64 (little & big-endian-correct code via explicit byte-order helpers).
- **NFR-X2** OS specifics only in `src/net/` behind interfaces.

### 3.6 Observability
- **NFR-O1** Structured logs (key=value or JSON), levels, no secrets/PII by default, sanitized.
- **NFR-O2** Metrics: connections (current/total/rejected by reason), bytes in/out, messages in/out, close codes histogram, handshake latency, queue depth, loop lag.
- **NFR-O3** Per-connection ID for correlation.

## 4. Default resource limits (all configurable, all enforced)

| Limit | Default | Violation behaviour |
|---|---|---|
| Max handshake size (request line + headers) | 8 KiB | 431 / close |
| Max header count | 64 | 431 / close |
| Handshake timeout | 5 s | close |
| Max frame payload | 1 MiB | close 1009 |
| Max message size (after reassembly/inflate) | 1 MiB | close 1009 |
| Max fragments per message | 1024 | close 1008 |
| Incomplete-message timeout | 30 s | close 1008 |
| Max outbound frame payload (auto-fragmentation size) | 64 KiB | — |
| Data-frame rate per connection | 10 000 / s | close 1008 |
| Max concurrent connections (global) | 10 000 | refused at accept: closed at once, no answer (FR-S1: minimal cost) |
| Max connections per IP | 100 | 429 / close |
| Handshake rate per IP | 50 / s | 429 |
| Outbound queue per connection | 1 MiB | slow-consumer policy (default: close 1008) |
| Inbound read buffer | 16 KiB per event loop, shared by its connections; per connection only the unfinished frame is held (nothing reserved up front, bounded by the frame limit) | — |
| Idle timeout | 60 s without any frame | close 1001 |
| Ping interval / pong timeout | 30 s / 10 s | close 1002/1001 |
| Close handshake timeout | 5 s | force TCP close |
| Control-frame rate | 100 / s / conn | close 1008 |
| Inflate ratio (permessage-deflate) | 100 : 1 | close 1009 |

## 5. Acceptance

A requirement is accepted when (a) a test cites its ID, (b) the test passes in CI, and (c) any related edge-case rows in PROTOCOL_EDGE_CASES.md are covered.
