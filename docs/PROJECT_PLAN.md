# Project Plan

Working name: **wsrv**. Owner: project lead. Status: planning.

## 1. Vision

A WebSocket server library + reference binary that a senior engineer would trust in production:
spec-correct, hard to misuse, hard to crash, easy to reason about, easy to extend.

**End goal (project lead):** an open-source, community-driven, well-documented socket library that
gives long-lived WebSocket capability to ecosystems that cannot do it natively — **PHP first**
(classic PHP cannot own sockets across requests) — and is equally usable from other languages
and, over time, other operating systems. One audited C++ core, a stable C ABI, thin per-language
bindings, and a sidecar daemon for stacks that cannot load native code. See [BINDINGS.md](BINDINGS.md),
ADR-0009, ADR-0010, ADR-0011.

## 2. Goals

1. Full **RFC 6455** server compliance, verified by the Autobahn|Testsuite (fuzzingclient) with zero failures in the core sections (1–10, 12–13 where extension enabled).
2. **Secure by default**: strict limits, bounded memory, Origin checking, TLS 1.2+ (1.3 preferred).
3. **Sans-I/O protocol core**: all protocol logic testable without sockets or clocks.
4. **Shared-nothing multi-reactor** scalability: ≥ 100k idle connections and line-rate echo on commodity hardware (targets in REQUIREMENTS.md).
5. **SOLID, pattern-driven design** with enforced module boundaries.
6. **Observability**: structured logs, metrics, tracing hooks.
7. **Maintainability**: documented decisions (ADRs), enforced style, ≥ 90 % line coverage in protocol layer, fuzzing in CI.

## 3. Non-goals (v1)

- Client implementation (the codec is symmetric so a client can come later; not in scope now).
- HTTP serving beyond the Upgrade handshake (no static files, no routing of general HTTP). Health endpoint only.
- HTTP/2 (RFC 8441) and HTTP/3 (RFC 9220) WebSockets.
- Windows/macOS support (the `Poller` abstraction keeps the door open; not built in v1).
- Clustering / cross-node pub-sub (hooks provided; no implementation).
- Application-level protocols (STOMP, Socket.IO, etc.). Subprotocol *negotiation* is supported; implementations are not.

## 4. Anti-goals (a "technically works" outcome we reject)

- Passes happy-path tests but crashes or leaks under malformed input.
- Unbounded buffers or queues anywhere.
- Protocol code that cannot run without a socket.
- Global mutable state or hidden singletons.
- A "God" `Server`/`Connection` class that parses, schedules, logs, and writes.
- Performance tricks that make code unreadable without benchmark evidence.

## 5. Key technical decisions (details in ADRs)

| Decision | Choice | ADR |
|---|---|---|
| Language level | C++23 (for `std::expected`, `std::span`, concepts, `std::byte`) | 0001 |
| Protocol core | Sans-I/O state machines, pure functions of bytes in → events/bytes out | 0002 |
| Concurrency | Reactor per thread, connections pinned to one loop, no shared connection state | 0003 |
| OS I/O | Linux `epoll` (edge-triggered) behind `IPoller` | 0004 |
| Dependencies | Zero runtime deps in core; SHA-1/Base64/UTF-8 implemented in-tree; OpenSSL only for optional TLS | 0005 |
| Errors | `Result<T,E>` (`std::expected`) on all runtime paths; exceptions only at startup/config | 0006 |
| Memory safety | Bounded buffers + backpressure everywhere | 0007 |
| Quality gates | Autobahn + fuzzing + sanitizers are release blockers | 0008 |

## 6. Milestones

Each milestone is *done* only when its exit criteria and the global Definition of Done (CONTRIBUTING.md) hold.

| # | Milestone | Deliverables | Exit criteria |
|---|---|---|---|
| M0 | Foundation | CMake presets, CI, clang-format/tidy configs, sanitizer presets, empty module skeletons, test harness | CI green on empty tree; all lint gates active |
| M1 | Base & crypto | `Result`, `ByteBuffer`/ring buffer, `Clock`, `ILogger`, SHA-1, Base64, CSPRNG wrapper, UTF-8 incremental validator | RFC 3174/4648 vectors pass; UTF-8 passes exhaustive + fuzz tests |
| M2 | Handshake | HTTP upgrade parser (incremental), validator chain, `Sec-WebSocket-Accept`, subprotocol & Origin policy, error responses (400/403/405/426) | Every handshake row in PROTOCOL_EDGE_CASES.md has a passing test; parser fuzzed 10 min clean |
| M3 | Framing | Frame parser/serializer (incremental), masking (SIMD-ready scalar first), message assembler, close-code validation | All framing rows tested; fuzz clean; round-trip property tests |
| M4 | Protocol state machine | `ProtocolSession` (sans-I/O): states, fragmentation, control frames, close handshake, ping/pong, limits → events | Deterministic scripted-session tests; ≥ 95 % branch coverage on `protocol/` |
| M5 | Networking | RAII sockets, epoll poller, event loop, timer wheel, acceptor, plain transport, wake-up channel | Loop tests (timers, wake-ups, EINTR/EAGAIN/EMFILE fault injection) pass |
| M6 | Single-thread server | `Connection`, handler API, write queue + backpressure, limits, graceful shutdown, echo example, **browser demo UI** (static page served by the example server: live connection/frame log, send text/binary/fragmented/ping, trigger close codes, show limits and errors, so the owner and newcomers can see how the system behaves) | **Autobahn core suite 100 % pass** (non-strict "INFORMATIONAL" allowed only for documented cases) |
| M7 | Multi-thread | N loops, `SO_REUSEPORT`/acceptor distribution, cross-thread `SessionHandle::post/send` | TSAN clean; soak test 1 h; scaling benchmark documented |
| M8 | TLS | `TlsTransport` decorator (OpenSSL), cert reload, SNI, hardened defaults | TLS interop tests; testssl.sh grade A; Autobahn over wss |
| M9 | permessage-deflate | RFC 7692 negotiation + streaming (de)compress with ratio/size limits | Autobahn sections 12–13 pass; decompression-bomb test passes |
| M10 | Pub/Sub & hardening | Topic broker, rooms, rate limiting, per-IP limits, metrics, chat example, 1.0-rc docs | Security review done; benchmarks published; release checklist complete |
| M11 | C ABI + `wsrvd` sidecar | `wsrv.h` (pull events, commands, handshake hook), ABI checker, `wsrvd` daemon, `IPC_PROTOCOL.md`, C-only consumer test | ABI check in CI; C consumer passes Autobahn; IPC fuzzed; fork/signal rules documented |
| M12 | PHP binding (Tier 1) | Composer package: sidecar client (all PHP stacks) + FFI binding (CLI); ReactPHP/Revolt adapters; Laravel/Symfony adapters later | PHP echo/chat examples; Autobahn through PHP binding; 5-minute quick start; PHP 8.1+ matrix |
| M13 | More bindings & platforms | Python, Node, Go, Rust bindings (Tier 1/2 per GOVERNANCE); macOS (`KqueuePoller`), then Windows | Each binding passes Autobahn in CI; per-OS CI green |
| M14 | Open-source launch | LICENSE/NOTICE, CoC, GOVERNANCE, templates, website/docs site, roadmap, good-first-issues, signed releases + SBOM | Public repo checklist complete; **1.0 tag** |

> **Status (2026-10-30):** M0-M6 merged (PR #1-#10). M5 = net (lesson 10). M6 = single-thread server: loop `defer`, signal watcher, `ServerConfig`, handler API, `HandshakeDriver`, `Connection`, `ConnectionTable`, `Server`, `ServerBuilder`, echo example, browser demo, adversarial integration tests, Autobahn harness (lesson 11, ADR-0013 Proposed). **Open exit criterion for M6: the first full Autobahn run** (`tests/autobahn/run.sh`; the image could not be pulled on the development machine). The M6 demo UI shows what a browser can observe (a browser cannot send Ping, fragments or invalid UTF-8; the plan's "fragmented/ping" probes are covered by the integration tests and Autobahn instead). **M7 (2026-10-30): multi-loop server, `SessionHandle`, streaming `MessageWriter` with `on_writable`, stress test, `wsrv_load` benchmark, 1 h soak (0 failures) merged (PR #12-#19, lessons 12-13, ADR-0013..0015 still Proposed).** Open: the 4-loop throughput plateau is unexplained (`docs/PERFORMANCE.md`). M8 (TLS) is next in the table; M11 (C ABI) is the prerequisite for any language binding.
>
> **Status (2026-11-02), M11 (C ABI, ADR-0016):** slices 1-3a merged (event queue and host bridge, read pause, `EmbeddedServer` and text configuration, PR #22-#24 and 3a). **Slice 3b done on branch `feat/capi-c-abi`: `include/wsrv/wsrv.h`, the `extern "C"` shim, `libwsrv.so` with a linker-enforced export list, C99 consumer test, lesson 15 (C ABI half).** **Sidecar (ADR-0017, accepted): slice 1 (`ipc/` frame codec + `fuzz_ipc_frame`), slice 2a (`net::UnixListener` with socket-file hygiene) and slice 2b (`apps/wsrvd`: configuration, token file, peer-uid check, single consumer, HELLO handshake with deadline, signals; `wsrvd --ipc-socket=PATH` runs and answers HELLO/PING) are done on branches `feat/capi-c-abi` and `feat/wsrvd-skeleton`.** **Sidecar slice 3 (2026-11-03, branch `feat/wsrvd-pump`): `IBackend` seam, `Relay`, event pump with two-way backpressure, stall timer, disconnect policy (1012 after grace, 1001 on stall; ADR-0017 amendment 4c), `IPC_PROTOCOL.md`, lesson 16, end-to-end tests with a real WebSocket client.** **M12 slice 1 (2026-11-03, branch `feat/m11-leftovers`): `bindings/php/`, a pure-PHP sidecar client (`Client`, `Worker`), unit and interop tests under CTest, lesson 17. Open for M12: Composer publication, PHP 8.1 CI matrix, ReactPHP/Revolt adapters, FFI binding, Autobahn through PHP.** **M11 leftovers (2026-11-03, PR #29): wsrvd operations guide with signal, exit-code and fork rules (`OPERATIONS.md` section 7), reusable C++ IPC test client (`tests/support/ipc_client.hpp`), ABI checks confirmed in CI (`capi_abi_symbols`, `capi_header_matches_symbols`, `capi_consumer` run in `ctest --preset dev`), ADR-0018 written and accepted.** **Handshake hook slice 1 (2026-11-03, branch `feat/handshake-hook-server`): `AwaitingDecision` phase, decision timer, `PendingGate`/`PendingSlot`, `IHandshakeDecider`, `PendingHandle`, `ServerBuilder::handshake_decider`, `Hangup` poll interest, lesson 18.** **Handshake hook slices 2-4 (2026-11-03/04, PRs #31, #32 and the PHP slice): C ABI 0.2 (`WSRV_EV_HANDSHAKE`, `wsrv_handshake_accept/reject`), IPC v0.2 with fail-closed intake in `wsrvd`, `fuzz_handshake_blob`, PHP `authenticate` callback with `Upgrade` and `HandshakeRequest`, interop test with a real cookie check, lessons 19-21; ADR-0018 is fully implemented.** Still open for M11: "C consumer passes Autobahn" (needs the Autobahn image). M12 (PHP binding) has not started. Open from M6: the first full Autobahn run.

> Sequencing note: TLS (M8) is required built-in (owner decision). permessage-deflate (M9) may slip behind M11/M12 if needed for the PHP milestone. Decide at M7 exit.

## 7. Work breakdown rules

- **Vertical slices inside layers, strict layering across them.** Lower layers are finished and fuzzed before higher layers depend on them.
- **Test first** for protocol code (write the edge-case test row, then the implementation).
- **One concern per PR**, ≤ ~400 changed lines excluding tests/generated.
- **ADR before architecture change.** No ADR → no merge of structural change.

## 8. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Subtle protocol bug (UTF-8 across fragments, close-code rules) | Interop failure / security | Autobahn from M6; fuzzers from M1; edge-case checklist is a test index |
| Memory/DoS exposure (slowloris, fragment floods, zip bombs) | Availability | Limits table (REQUIREMENTS §4) enforced + tested; per-connection and global budgets |
| epoll edge-trigger starvation / missed events | Hangs | Read until EAGAIN with fairness budget; loop tests with fault injection |
| Cross-thread lifetime bugs | UAF | `SessionHandle` = weak ref + loop-posted tasks only; TSAN/ASAN in CI |
| TLS integration complexity | Schedule | TLS is a decorator on `ITransport`; isolated behind M8 |
| Scope creep | Schedule | Non-goals list; ADR + plan update required to add scope |
| Toolchain drift | Build breaks | Pinned compilers in CI matrix; documented minimums |

## 9. Quality gates (summary)

Compiler warnings as errors · clang-tidy · clang-format · unit + integration tests · ASAN/UBSAN · TSAN (threaded) · fuzz smoke (60 s per target per PR, long runs nightly) · Autobahn · coverage thresholds · dependency/license check. Detail: [TESTING_STRATEGY.md](TESTING_STRATEGY.md), [BUILD_AND_TOOLING.md](BUILD_AND_TOOLING.md).

## 10. Open questions (owner decision needed)

Resolved (2026-10-24):
- License: **Apache-2.0** (ADR-0011). `LICENSE` added.
- "Other platforms" = **both** other languages and other operating systems (BINDINGS §5–6).
- TLS: **built in** (in-process, M8 stays a core milestone; the "slip behind M11/M12" note in §6 no longer applies).
- PHP deployments: owner delegated; plan covers all three — sidecar for PHP-FPM first, FFI/extension for CLI workers second, framework adapters (Laravel, Symfony) third.
- Packet level: the project parses and builds WebSocket frames itself over raw TCP (no HTTP/WS library). Raw IP/TCP packet handling is out of scope (the kernel owns it).
- Working style: every step is taught (what / why / how) — see [learning/README.md](learning/README.md).

Still open:
1. Final project name (`wsrv` is a placeholder) and repo host.
2. CI provider (GitHub Actions assumed).
3. Target scale: confirm numbers in REQUIREMENTS §3.
4. Should a WebSocket **client** be a v1.x goal?
