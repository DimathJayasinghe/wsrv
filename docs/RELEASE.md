# Release readiness: 1.0-rc

> **0.1.0 (2026-11-23)** is the first release meant for developers to build with: everything in section 2 holds for it, and the limits in section 3 apply. It is not the 1.0: the API may change in a minor version, and the unchecked boxes in section 4 are the gap. Pin the version you use.

What a 1.0 release candidate claims, the evidence for each claim, what is known to be missing, and the checklist that decides it. Written at the end of M10 (2026-11-14). A claim without evidence in the right-hand column is not a claim.

## 1. What the release candidate is

A C++23 RFC 6455 WebSocket server core: sans-I/O protocol, multi-loop server on epoll, TLS (OpenSSL), permessage-deflate (RFC 7692), per-IP and global limits, a pub/sub broker, Prometheus metrics, a stable C ABI (`libwsrv`), the `wsrvd` sidecar and a pure-PHP client. Linux only. The C++ headers still live in `src/server/` until the C ABI fixes the public surface (ADR-0013); the C ABI (`include/wsrv/wsrv.h`) is the stable part.

## 2. Evidence

| Claim | Evidence | Where |
|---|---|---|
| Conforms to RFC 6455 and 7692 | Autobahn|Testsuite sections 1-7, 9, 10, 12, 13: 517 cases, 0 FAILED, also over `wss://`; the excluded cases and why | `tests/autobahn/run.sh`, `tests/autobahn/EXCLUDED.md` |
| Every edge case has a test | The edge-case index maps each row to tests; status table kept per milestone | `docs/PROTOCOL_EDGE_CASES.md` sections 9-10 |
| Hostile input does not crash or leak | 12 fuzz targets (frame parser, handshake, HTTP, UTF-8, message pipeline, session, inflate, IPC frame, handshake blob, TLS server, SHA-1, Base64), built and run with the sanitizers in CI | `tests/fuzz/`, CI job "clang (libc++, dev, tsan, fuzz)" |
| No data races | The whole suite under ThreadSanitizer in CI; multi-thread stress tests | `ctest --preset tsan`, `MultiThreadStress.*`, `BrokerTest.*Threads*` |
| No memory errors | The whole suite under ASan and UBSan in CI (1248 tests at this writing) | `ctest --preset dev` |
| Static analysis clean | clang-tidy with warnings-as-errors on every file, banned-function and module-boundary checks | CI job "clang-tidy (libc++)", `tools/check_*` |
| No leaks over time | 1 hour soak, 32 churning connections: descriptors flat (78-86), resident memory flat after warm-up | `docs/PERFORMANCE.md` "Soak" |
| Scales across loops, fan-out cost known | Scaling table (1 to 8 loops) and pub/sub fan-out table, with their limits stated | `docs/PERFORMANCE.md` |
| TLS is hardened | testssl.sh Overall Grade A+, Autobahn over wss 0 FAILED | ADR-0019 amendment 6 |
| Resource limits hold | Per-IP caps and rate (ADR-0021), descriptor budget (ADR-0022), bounded every queue and table, decompression bombs refused (ADR-0020), admin port bounded (ADR-0023), broker bounds (ADR-0024) | `docs/SECURITY_DESIGN.md` threat table, tests named there |
| Security reviewed | Three independent read-only reviews: M9 slices (PR #42), the second review (PR #43), the M10 slices (PR #50); findings fixed or recorded with a decision | section 3, the ADR amendments |
| The C ABI is stable and checked | Exported symbols list enforced by the linker, header matches symbols, a C99-only consumer test, all in `ctest` | `capi_abi_symbols`, `capi_header_matches_symbols`, `capi_consumer` |
| Observable | `/metrics`, `/healthz`, `/readyz` on a separate listener, closed set of counters | `docs/OPERATIONS.md` section 3 |

## 3. Known limitations (stated, not hidden)

Each is either a documented decision or a follow-up with an owner decision pending.

- **Linux only** (epoll). macOS and Windows are M13.
- **Rooms from PHP work through the sidecar only.** `wsrvd` with `--broker=true` gives a PHP worker `Connection::subscribe` and `Worker::publish`, and any process can publish with `Wsrv\Publisher` over the HTTP endpoint (ADR-0028). The C ABI (`wsrv_publish` / `wsrv_subscribe`) and so the PHP-FFI embedded mode do not have rooms yet (ADR-0027 slice 2, planned as ABI 0.5).
- **permessage-deflate multiplies broadcast cost** per subscriber (ADR-0024 A2). Leave compression off for broadcast-heavy deployments, or lower `max_payload_bytes`.
- **Broker bounds are global**, not per user (ADR-0024 A4): an application serving strangers adds its own quota.
- **The admin listener has no authentication** and no `Host` check (SECURITY_DESIGN T21); keep it on loopback or behind a network policy.
- **Metrics are counters and gauges only.** The histograms (`handshake_seconds`, `loop_lag_seconds`, `outbound_queue_bytes`) in OPERATIONS section 3 are planned; there is no loop-lag watchdog yet (SECURITY_DESIGN T16). **Decision: post-1.0**; the handlers run on the host thread, so a slow handler cannot stall an I/O loop.
- **From the second security review, still open (owner delegated the decisions to the assistant on 2026-11-22; items 1, 2, 3 and 5 of that review were fixed in #43):** (a) `SessionHandle` sends copy whole messages into posted closures, bounded only by the task-queue size (broadcasts have a reserved lane, direct sends do not). The sender is the application's own code, not a peer, so this is a host-driven risk: **accepted for the release candidate, to be built before 1.0** as a per-loop-link byte budget. (b) No global buffered-bytes budget across connections (about 3 MiB per connection worst case with deflate, OPERATIONS 1.1): **accepted for 1.0 with the sizing formula documented; a budget key is post-1.0**. (c) The zlib state of a connection lives for the connection once used: **accepted**, it is bounded by the window setting.
- **`IpLimits::sweep`** scans the whole table under its mutex on the accept path when the table is full (low risk at the default rates). **Accepted; amortise it when a benchmark or a report shows it matters.**
- **Per-IP limits can be switched off** with `0`. **Decision (2026-11-22): confirmed as the one exception** to "no unlimited value": behind a reverse proxy or NAT every user is one address, and the defaults would refuse real users. They are on by default, and the C keys of ADR-0016 A1 refuse `0` (no "off" there).
- **PHP binding**: the sidecar client and worker (with rooms and the HTTP `Publisher`), and the embedded FFI server (`Wsrv\Embedded\Server`, ADR-0025, with the `authenticate` handshake hook, no rooms yet). Examples: `examples/chat`, the Docker quickstart, and `docs/LARAVEL.md` (tried by hand against Laravel 13, not in CI). Not done: loop adapters (ReactPHP, Revolt) and a Laravel Broadcasting driver. Published: `wsrv/client` on Packagist, `wsrvd` images on GHCR, checksummed binaries with build attestation on GitHub Releases (ADR-0026; `0.1.0-rc.2` was verified end to end from those artifacts on 2026-11-23). The PHP tests run on PHP 8.2 (the floor) and the latest release in `.github/workflows/php.yml`. The C consumer and the PHP binding pass Autobahn (run by hand with `tests/autobahn/run.sh --capi` and `--php`, not in CI: the 1.5 GB suite image is not part of the CI cost design).
- **Single-machine numbers** (PERFORMANCE.md): comparisons between loop counts, not capacity planning. There is no CI speed gate, by decision (PERFORMANCE.md "Regression gates").

## 4. Release checklist

Tick only with evidence. "Owner" items need a decision that is not the assistant's.

Engineering
- [x] All CI jobs green on main (gcc dev and release with LTO, clang with libc++ and TSan and fuzz, clang-tidy)
- [x] Autobahn: 0 FAILED (517 cases), plain and wss
- [x] Fuzzers run clean in CI; corpus committed
- [x] Soak run of one hour passes (`wsrv_load --churn`)
- [x] Benchmarks published with their limits (`docs/PERFORMANCE.md`)
- [x] Security review of the last milestone done, findings fixed or recorded
- [x] The default limits of REQUIREMENTS section 4 exist in the configuration and are enforced (index: PROTOCOL_EDGE_CASES section 10)
- [ ] A row-by-row audit of REQUIREMENTS section 4 against its tests for 1.0 (spot-checked, not exhaustive)
- [x] Operations guide covers config, metrics, shutdown, hardening, runbook (OPERATIONS)
- [x] ABI checks in CI
- [ ] Coverage re-measured against the NFR-M4 thresholds for 1.0 (`coverage` preset exists; no figures were re-collected for this document)
- [x] Autobahn through the C consumer (`examples/echo_c`, plain and wss, 517 cases, 0 FAILED; `tests/autobahn/EXCLUDED.md`)
- [x] Autobahn through PHP (`bindings/php/examples/embedded_echo.php`, plain and wss, 517 cases, 0 FAILED; `tests/autobahn/EXCLUDED.md`)
- [x] The open items of section 3 either fixed or accepted (decisions of 2026-11-22 above; the one build-before-1.0 item is the send byte budget, and the permessage-deflate answers are in ADR-0020 A2)
- [x] ADR-0020 A2 items 1 and 3 implemented with their tests (EC-D08, EC-D09), and a test for the release of the send scratch buffer (EC-D05)

Repository and launch (M14, listed here so nothing is forgotten)
- [x] Owner: final project name (`wsrv`, confirmed 2026-11-22; Composer name `wsrv/client`, found free on Packagist) and license confirmed
- [ ] `NOTICE`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md` added
- [ ] Owner: review of the git history (author e-mail addresses) and a full-history secret scan before the repository goes public
- [ ] Owner: branch protection on `main`; secret scanning, push protection, private vulnerability reporting enabled
  - Recommendation (2026-11-22): block force pushes and branch deletion on `main`; do **not** require status checks while `ci.yml` uses `paths-ignore`, because a docs-only PR gets no status and would stay blocked forever. Required checks are an M14 decision. Not applied: a repository setting, and a private repository may need a paid plan for it.
- [ ] Signed release artifacts and an SBOM
- [ ] Versioning policy stated: the C ABI follows semantic versioning; the C++ headers are not stable before they move to `include/wsrv/`

## 5. Go / no-go

**Release candidate for evaluation: yes.** The server core, TLS, deflate, limits, broker, metrics, C ABI and sidecar are built, tested under every sanitizer, fuzzed and reviewed three times; the claims in section 2 can be checked by running the named commands.

**Public 1.0: not yet.** The unchecked boxes above are the gap. The engineering boxes are small (coverage figures, the three open review items); the repository boxes are owner decisions and the M14 work. This document does not change repository visibility; that is the owner's call.

## 6. Going public (checklist, 2026-11-22)

Done in the repository: `LICENSE`, `NOTICE`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md`, `GOVERNANCE.md`, `SECURITY.md` (reports through GitHub's private advisory), `CONTRIBUTING.md` with a "Where to start" list, issue and PR templates (blank issues off, security link), README status banner, scan of the history and the tree for secrets and personal data (patterns only; no key, token or certificate found), branch protection on `main` (no force push, no deletion), repository-local commit email set to the GitHub noreply address.

Owner decisions that cannot be made for the owner:
- [x] **The project name**: `wsrv`, Composer package `wsrv/client` (owner, 2026-11-22; Packagist showed no `wsrv/` vendor).
- [ ] **Commit history.** All commits carry the owner's personal email (`git log --format=%ae`). Rewriting history would break every PR and merge link; the alternative is to accept it. New commits use the noreply address. In GitHub, also tick "Block command line pushes that expose my email" (account settings, Emails).
- [ ] **Flip the visibility** (settings, danger zone), or tell the assistant explicitly to.

Right after the flip (not available while the repository is private):
- [ ] Settings, Code security: enable private vulnerability reporting, secret scanning, push protection, Dependabot alerts.
- [ ] Confirm the Security tab shows "Report a vulnerability" (SECURITY.md and the issue template link depend on it).
- [ ] Push an `-rc` tag to run `release.yml` for the first time (the arm runner and the attestation need a public repository), then submit the repository URL on packagist.org.
- [ ] After the first tag that builds the container image: make the GHCR package `wsrvd` public (package page, settings, danger zone; a new package is private, so `docker pull` fails for everyone else) and link it to the repository.
- [ ] Optionally create the first "good first issue" labels and issues from CONTRIBUTING.md's list.
