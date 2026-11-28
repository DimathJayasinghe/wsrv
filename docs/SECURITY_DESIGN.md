# Security Design

Security is a design property, not a phase. This document is the threat model and the control catalogue. Reporting policy: [../SECURITY.md](../SECURITY.md).

## 1. Assets & trust boundaries

| Asset | Why it matters |
|---|---|
| Server availability | Primary DoS target |
| Process memory integrity | Remote code execution if corrupted |
| Application data on connections | Confidentiality/integrity (esp. with TLS) |
| Credentials/tokens in handshake | Auth bypass, session hijack |
| Logs / metrics | Information leak, log injection |

Trust boundaries: **network ↔ transport** (fully untrusted bytes), **server ↔ application handler** (handler is trusted code but may be buggy), **config/cert files ↔ process** (trusted, but validate).

## 2. Threats (STRIDE-oriented) and controls

| # | Threat | Vector | Control | Verified by |
|---|---|---|---|---|
| T1 | Memory corruption (overflow, OOB, UAF) | Malformed frames/headers, length fields | Bounds-checked spans, checked arithmetic, no raw pointers, RAII, sanitizers, fuzzing | Fuzz targets, ASAN/UBSAN |
| T2 | Slowloris (handshake) | Trickle header bytes | Handshake timeout, size limits, min-rate rule, per-IP caps | Integration test |
| T3 | Slow read / write stall | Peer never reads | Bounded outbound queue, slow-consumer policy, write timeout | Integration test. **As built (M6):** `max_queued_bytes`, `SlowConsumerPolicy` (Close 1008 / DropMessage), `write_stall_timeout`; `ConnectionTest.ASlowConsumer*`, `APeerThatAcceptsNoBytes*`, `Integration.ASlowReader*` |
| T4 | Connection flood / FD exhaustion | Many sockets | Global & per-IP caps, `EMFILE` reserve fd, accept backoff, `RLIMIT_NOFILE` check at start | Load test |
| T5 | Memory exhaustion via large message | Huge length, many fragments | Frame/message/fragment limits, lazy buffer growth, never pre-allocate declared length | Edge-case tests |
| T6 | Decompression bomb | permessage-deflate | Inflate output cap + ratio limit, streaming inflate, `no_context_takeover` option | Bomb test |
| T7 | CPU exhaustion | Tiny frames flood, ping flood, zero-length frames, pathological UTF-8 | Per-connection rate limits, work budget per loop iteration (fairness), O(n) parsers only | Benchmark + test |
| T8 | Cross-Site WebSocket Hijacking (CSWSH) | Malicious page opens WS using victim's cookies | Mandatory Origin allow-list (secure default), SameSite cookie guidance, token-in-first-message or custom header auth recommendation | Handshake tests |
| T9 | Header injection / response splitting | CR/LF in values echoed back | Strict parser (reject bare CR/LF/NUL), never echo raw peer values; sanitise selected subprotocol (token grammar) | Edge-case tests |
| T10 | HTTP request smuggling / desync | Ambiguous framing in handshake | Reject requests with body (`Content-Length`>0, `Transfer-Encoding`), reject duplicate `Host`/`Content-Length`, single request per connection before upgrade | Edge-case tests |
| T11 | Protocol confusion | Non-WS client after handshake | Strict frame validation; fail closed (1002) | Autobahn |
| T12 | TLS downgrade / weak crypto | Old protocols/ciphers | TLS ≥ 1.2, AEAD ciphers only, no compression, no renegotiation, OCSP stapling optional, `SSL_OP_NO_*` set | testssl.sh in CI |
| T13 | Information disclosure | Verbose errors, logs, stack traces | Generic client errors; detailed server logs only; secrets never logged; log sanitising | Review + tests |
| T14 | Log injection | Peer strings in logs | Escape control chars / quote values, length-cap | Unit test |
| T15 | Resource leak over time | Error paths forgetting cleanup | RAII, soak tests with fd/RSS monitoring | 24 h soak |
| T16 | Malicious/buggy handler | Exceptions, blocking, retaining dangling refs | Exception barrier, loop-lag watchdog metric, API lifetimes documented, `SessionHandle` weak semantics | Tests. **As built (M6):** exception barrier (`Connection::guarded`, close 1011, server continues) and documented `Session` lifetimes; `ConnectionTest.AThrowingHandler*`, `Integration.AThrowingHandler*`. **As built (M7):** `SessionHandle` holds an id and a guarded `LoopLink`, never a connection pointer, so a kept handle cannot dangle (`SessionHandleTest.*`, `ServerHandle.*`, `MultiThreadStress.*` under TSan); a `MessageWriter` is cleared by its connection (`ConnectionTest` streaming section). **As built (ADR-0032):** direct sends waiting for a loop are bounded in bytes (`max_pending_send_bytes`, default 64 MiB per loop; `BudgetTest.*`). The loop-lag watchdog metric (M10) is not built yet |
| T17 | Timing side channels | Token comparison in auth hook | Provide `constant_time_equal` helper; docs | Unit test |
| T18 | Supply chain | Compromised dependency | Minimal deps, pinned hashes, SBOM, signed tags/releases, dependabot-equivalent | CI |
| T19 | Privilege abuse | Running as root | Documented drop-privileges, systemd hardening, seccomp profile (optional) | Ops doc |
| T20 | Predictable randomness | Weak masks/IDs | `getrandom(2)` only; no `rand()` | Banned-function check |
| T22 | Publish endpoint abuse (ADR-0028) | Anyone who can reach the publish port: guessing the token, forging messages, request smuggling, memory pinning with a huge `Content-Length`, slow clients, flooding the rooms | Off unless `publish_port` is set; needs `broker=true` and a token file (mode 0600, owned by the service user; the token must be one `wsrvd --generate-token` made, from 1.1, ADR-0039: 256 random bits in a fixed form with a checksum, because a length floor cannot tell a random token from a guessable one; no back-off, which would let anyone lock the real publishers out; there is no key, flag or config value for the secret); bearer token compared in constant time; order of checks: head, framing, **token**, length bound, only then a body buffer (an unauthenticated peer reserves nothing); `Transfer-Encoding`, repeated or non-canonical `Content-Length`, `Expect` and bytes after the body refused; no percent-decoding; head <= 4 KiB, one 5 s deadline for head and body, at most 16 clients, one request per connection, body <= `broker_max_payload_bytes`; publish-only (no read or list endpoints); every answer counted in `wsrv_publish_http_total{status}` | `PublishRequest.*`, `PublishHttp.*`, `PublishServer.*`, `EmbeddedServer.*Publish*`, `fuzz_publish_request` (ADR-0028). **Known, accepted:** plain HTTP, so the token travels in clear text on a non-loopback bind (the daemon warns at start; put a TLS proxy or a private network in front); a leaked token publishes to every room (no per-topic permissions in 1.0); no per-token rate limit |
| T21 | Admin port abuse / information disclosure | Anyone who can reach the admin port: floods, slow requests, scraping counters | Off unless configured; loopback by default (warn on anything else); plain GET of three fixed pages, exact-match targets, head <= 2 KiB, 5 s deadline, at most 8 clients at once, one non-blocking send, no body read, no keep-alive; closed enumeration of metrics (no peer data in any label) | `AdminHttp.*`, `AdminServer.*` (ADR-0023). **Optional token (1.1, ADR-0041):** with `admin_token_file` set, `GET /metrics` needs `Authorization: Bearer <token>` (a token `--generate-token` made, different from the publish and IPC tokens), compared in constant time before the page is rendered, with one `401` answer for a missing, wrong, malformed or repeated credential (`AdminHttpToken.*`, `AdminServerToken.*`, `BearerCredential.*`); `/healthz` and `/readyz` stay open (probes cannot send a credential). Unset, nothing changes. **Known, accepted (M10 review; the token closes only the counters part):** eight slow clients, renewed every 5 s, can hold every admin slot and make `/healthz` unanswerable, per-IP limits do not apply to this listener, and there is no `Host` check (a DNS-rebinding page can read the counters on a loopback bind unless the token is set; it cannot guess the token): keep it on loopback or behind a network policy, as OPERATIONS 3.1 says. The token is plaintext on the wire (no TLS on this listener), has no lockout (256 bits; a lockout would let an attacker lock monitoring out) and does not touch the eight-slot exhaustion, because the request head is read before the credential is judged |
| T24 | Memory exhaustion by many connections that each hold buffered bytes (ADR-0040) | Many clients that each send the first fragments of a big message and stop (the attacker pays only the bytes it sends), or slow readers while the application broadcasts; per-address limits do not help behind a proxy or from many addresses | Every connection is bounded (`max_message_size`, `max_queued_bytes`); the **sum** is bounded by `max_buffered_bytes` (default 1 GiB): above it new clients get a 503 with `Retry-After` at accept, before a per-IP slot is taken, and existing connections are untouched; a per-connection ledger charges by difference and refunds from its destructor, so the counter cannot drift; counter `rejected_buffer_budget`, gauge `wsrv_buffered_bytes`, warn event `buffer_budget`. **Not covered:** a hard cap that closes live connections, and memory outside the two buffers (deflate state, bounded by `max_connections`) | `BufferBudgetServer.*`, `BufferLedger.*`, `ConnectionTest.*Charge*`, `Session.Buffered*` |
| T23 | IPC port abuse over TCP (ADR-0038) | Anyone who can reach the TCP IPC port: guessing the token, sniffing the plaintext link, connecting and saying nothing to lock the application out, flooding connections | Off unless `ipc_port` is set; the token is mandatory and must be one `wsrvd --generate-token` made (256 random bits in a fixed form with a checksum; a length floor cannot tell a random token from `MyCompanyWebsocketToken2026!!!!!`; the only authentication: there is no peer uid), compared in constant time, one `INVALID_ARGUMENT` for every failure, never logged, no back-off (it would let anyone lock the real worker out); loopback by default and a `warn` (`ipc_plaintext`) on any other bind; connections that have not said HELLO wait in a room of 8, each with its own deadline, the first accepted HELLO wins and the oldest is evicted when a ninth arrives, so keeping the real worker out takes more than 8 connections inside its connect-to-HELLO window (an authenticated application is never displaced); keepalive frees a vanished host's slot in about a minute; the link is NOT encrypted, so loopback, a private network, a VPN or an SSH tunnel only (documented, not enforced). A flood at that rate can still force the application to retry; it is mitigated, not cured, and only who can reach the port removes it: the port is a trust boundary like a database port | `IpcServer` tests over real TCP (token, displacement, ordering, keepalive options), container smoke test from the host |
| T25 | Pusher gateway abuse (ADR-0042, in the PHP worker) | Any browser that can open a WebSocket to the daemon: joining a private or presence channel it was not allowed to (a forged, copied or replayed signature), joining as another presence user, whispering as someone else, flooding frames at a single-threaded PHP worker, opening channels without bound, filling the daemon's room table with invented public channel names so that signed users are refused (measured: 16 connections from one address, 0.07 s, on the defaults; fixed in ADR-0042 A3), sending a value that crashes the encoder (`1e999`), repeating subscribes to a large presence room, or making a member list too big for the daemon so that the worker's link, and every browser, is lost | A signature `key:HMAC-SHA256(secret, socket_id:channel[:channel_data])` compared with `hash_equals`; the socket id is inside it, so a copy from another connection fails, and for a presence channel the user id and info are inside it, so a browser cannot join as another user; a whisper is built by the server from the signed user id, only for members, never on an encrypted channel; one token bucket for all frames (burst and sustained rate), a separate whisper rate, and strikes (forgiven by quiet time, one per full minute) that close with 1008; a per-connection channel quota that every join counts against, which bounds the joins in flight; a budget on the distinct public names held (`max_public_channels`, 512) that leaves the rest of the daemon's table to private and presence rooms, of which a quarter (`public_channels_reserved`, 128) only a connection with a verified signature may take, a public join that comes first waiting 5 seconds for it (ADR-0042 A4), and an optional list of the public channels the site has (`public_channels`); bounds on frame size, on `channel_data` and on members per channel; a value JSON cannot re-encode is a bad frame, not an exception; a send over the daemon's `ipc_max_frame` fails that send or join (`MessageTooLarge`) and never the link; a repeated presence subscribe is answered from at most 16 kept lists of at most 1 MiB, dropped when the members change; the secret is hidden from `print_r`, `var_dump` and `serialize`; the statistics line is numbers only (a browser's string never reaches a log) | `PusherCoreTest.*`, `PusherGatewayTest.*` (the security rules were checked by hand with deliberate breakages, each of which a test caught; there is no mutation script in the repository), `php_pusher_interop` (a real `wsrvd`), the load and churn runs in `bindings/php/tests/gateway_load`, `laravel_check.php`. **Known, accepted:** a visitor can still use up the part of the public budget that is not kept back (384 of 512) and keep new public names closed to other anonymous visitors until they leave, and a visitor with an account counts as signed in (only an exact `public_channels` list removes that; the guide says so); the app key is public by design (the signature protects a channel, and `check_app_key` is off by default); the per-address limits are the daemon's, and behind a proxy without `trusted_proxy` every browser is one address; presence is held in one worker for one daemon (no backplane); who may join a channel is decided by the application's own `Broadcast::channel` callbacks, outside this gateway |

## 3. Secure defaults ("deny unless allowed")

- Origin: header present and no allow-list ⇒ **reject** unless `allow_any_origin()` explicitly set (name is deliberately loud; logs a startup warning).
- Limits (REQUIREMENTS §4) are **on** by default; there is no "unlimited" value — maximums are capped by compile-time hard ceilings (e.g. frame ≤ 64 MiB) to prevent misconfiguration. **One exception, decided in ADR-0021:** the two per-IP limits accept `0` = off, because behind a NAT or a PROXY-protocol-only balancer every user is one address and the defaults would refuse real users (behind an HTTP reverse proxy, list it in `trusted_proxy` instead: ADR-0029); they are on by default and the ceilings still apply. (Confirmed 2026-11-22: the project owner delegated the question to the maintainer assistant, who confirmed the exception; the C configuration keys added in ADR-0016 A1 do not have it.)
- Plaintext listeners log a startup notice when bound to non-loopback without TLS (info, not error — a reverse proxy may terminate TLS).
- Server header / version string not disclosed by default.
- `permessage-deflate` is **off** by default (compression + secrets in the same message ⇒ CRIME/BREACH-class risk); enabling documented with the caveat.
- Error responses carry no body details.

## 4. Secure-coding rules (reviewer checklist)

1. Every wire-derived integer is range-checked **before** use as size/index/allocation.
2. No allocation sized from untrusted data without a configured upper bound.
3. No unbounded loop over peer-influenced data; per-iteration work budget.
4. No trust in peer-supplied strings in logs, headers, filenames, or commands.
5. All syscalls checked; `EINTR` retried; `EAGAIN` handled; `EPIPE`/`ECONNRESET` mapped to clean close.
6. File descriptors `CLOEXEC`; sockets non-blocking; `MSG_NOSIGNAL` on send.
7. Secrets (TLS private keys, tokens) never logged; keys loaded with restrictive permission check; memory zeroed with `explicit_bzero` where practical.
8. No `rand()`, `strcpy`, `sprintf`, `gets`, `strtok`, `atoi`, `system`, `popen` (banned-function check).
9. Any `unsafe`-ish code (`reinterpret_cast`, SIMD intrinsics, `memcpy` of structs) is isolated in a small audited file with its own tests and a `// SAFETY:` justification.
10. State-machine transitions are exhaustive (`-Wswitch-enum`); unknown state ⇒ close, never "continue".
11. Fail closed: on any parser doubt, close with the most specific code (1002/1007/1009) and stop reading.

## 5. TLS hardening profile (M8)

- Protocols: TLS 1.3, TLS 1.2 (AEAD ECDHE suites only). SSLv2/3, TLS 1.0/1.1 disabled.
- `SSL_OP_NO_COMPRESSION`, `SSL_OP_NO_RENEGOTIATION`, `SSL_OP_CIPHER_SERVER_PREFERENCE`, session tickets configurable (rotating keys).
- Certificates: validate chain/key match at load; reload on `SIGHUP`/file watch with atomic swap; failure keeps the old cert and raises an alert metric.
- Non-blocking handshake integrated with the loop; handshake timeout shared with HTTP handshake budget.
- Proper `close_notify` and mapping of TLS errors to `Error{Tls}`.

## 6. DoS budget model

Each connection has **budgets**: bytes buffered, messages/sec, control frames/sec, handshake time, idle time. The server has global budgets: connections (`max_connections`, and per address, ADR-0021); total buffered bytes (`max_buffered_bytes`, ADR-0040: a soft cap that sheds **new** connections with a 503 and leaves the existing ones alone; counts partial inbound messages and unwritten output); and, for fairness between connections, a read budget per readiness event (`ReadBudget`, EC-N07, EC-N08) in place of a per-tick CPU budget. Exhausting a budget has a defined, tested outcome (close code + metric + log). "Nothing happens" is not an outcome.

## 7. Security testing

- **Fuzzing** (libFuzzer + ASAN/UBSAN): `fuzz_http_request`, `fuzz_publish_request`, `fuzz_admin_http` (the metrics page is never rendered without the token, ADR-0041), `fuzz_frame_parser`, `fuzz_protocol_session`, `fuzz_utf8`, `fuzz_base64`, `fuzz_deflate_session`. Structure-aware mutators for frames. Corpus in repo; 60 s per PR, ≥ 1 h nightly; crashes become regression tests.
- **Autobahn** full suite (ws and wss) every PR to main.
- **Adversarial integration tests**: slowloris, slow reader, fragment flood, ping flood, connection churn, bomb.
- **Static analysis** (clang-tidy, cppcheck, `-fanalyzer`, CodeQL optional).
- **Pre-release**: manual security review using this document; threat model re-validated; dependency audit.

## 8. Incident handling

Private disclosure via SECURITY.md → triage within 3 business days → fix on private branch → regression test + fuzz corpus → coordinated release with advisory and CVE if warranted.

## 9. Change control

Any PR touching `http/`, `protocol/`, `net/`, `extensions/`, TLS, or limit defaults requires a reviewer to tick the "Security review" box in the PR template and update this document if threats/controls change.
