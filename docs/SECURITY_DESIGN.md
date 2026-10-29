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
| T16 | Malicious/buggy handler | Exceptions, blocking, retaining dangling refs | Exception barrier, loop-lag watchdog metric, API lifetimes documented, `SessionHandle` weak semantics | Tests. **As built (M6):** exception barrier (`Connection::guarded`, close 1011, server continues) and documented `Session` lifetimes; `ConnectionTest.AThrowingHandler*`, `Integration.AThrowingHandler*`. The watchdog metric (M10) and `SessionHandle` (M7) are not built yet |
| T17 | Timing side channels | Token comparison in auth hook | Provide `constant_time_equal` helper; docs | Unit test |
| T18 | Supply chain | Compromised dependency | Minimal deps, pinned hashes, SBOM, signed tags/releases, dependabot-equivalent | CI |
| T19 | Privilege abuse | Running as root | Documented drop-privileges, systemd hardening, seccomp profile (optional) | Ops doc |
| T20 | Predictable randomness | Weak masks/IDs | `getrandom(2)` only; no `rand()` | Banned-function check |

## 3. Secure defaults ("deny unless allowed")

- Origin: header present and no allow-list ⇒ **reject** unless `allow_any_origin()` explicitly set (name is deliberately loud; logs a startup warning).
- Limits (REQUIREMENTS §4) are **on** by default; there is no "unlimited" value — maximums are capped by compile-time hard ceilings (e.g. frame ≤ 64 MiB) to prevent misconfiguration.
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

Each connection has **budgets**: bytes buffered, messages/sec, control frames/sec, handshake time, idle time. The server has global budgets: connections, total buffered bytes (soft cap → shed new connections), CPU time per loop tick (fairness). Exhausting a budget has a defined, tested outcome (close code + metric + log). "Nothing happens" is not an outcome.

## 7. Security testing

- **Fuzzing** (libFuzzer + ASAN/UBSAN): `fuzz_http_request`, `fuzz_frame_parser`, `fuzz_protocol_session`, `fuzz_utf8`, `fuzz_base64`, `fuzz_deflate_session`. Structure-aware mutators for frames. Corpus in repo; 60 s per PR, ≥ 1 h nightly; crashes become regression tests.
- **Autobahn** full suite (ws and wss) every PR to main.
- **Adversarial integration tests**: slowloris, slow reader, fragment flood, ping flood, connection churn, bomb.
- **Static analysis** (clang-tidy, cppcheck, `-fanalyzer`, CodeQL optional).
- **Pre-release**: manual security review using this document; threat model re-validated; dependency audit.

## 8. Incident handling

Private disclosure via SECURITY.md → triage within 3 business days → fix on private branch → regression test + fuzz corpus → coordinated release with advisory and CVE if warranted.

## 9. Change control

Any PR touching `http/`, `protocol/`, `net/`, `extensions/`, TLS, or limit defaults requires a reviewer to tick the "Security review" box in the PR template and update this document if threats/controls change.
