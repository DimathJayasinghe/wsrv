# Release readiness: 1.0-rc

> No stable version has been released yet: the latest published version is the release candidate **`1.0.0-rc.2`** (2026-11-26). The sources are at **1.0.0** since 2026-11-25 (project, PHP package and C ABI 1.0, ADR-0016 A4). It is published first as release candidates (`v1.0.0-rc.N`); the final `v1.0.0` waits for the owner's go. The unchecked boxes in section 4 are what is left. Until the final release, pin the version you use.

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
| Survives restarts and outages | Chaos soak of the release image, 2 hours, 200 reconnecting JavaScript clients (on `node:22-slim`): 162 faults (37 graceful and 32 `kill -9` daemon restarts, 34 worker restarts and 29 worker kills, 30 network cuts of 3 to 10 s). After every fault all 200 clients were connected again; 26 400 reconnects in total; descriptors flat (217), daemon memory flat (4.0 MB at warm-up, 3.8 MB at the end), 0 replies that were never asked for, the daemon never crashed. It found and fixed a client bug on the way (ADR-0031 A1) | `tests/soak/` (`python3 tests/soak/soak.py --image <wsrvd image> --hours 2 --clients 200`) |
| Scales across loops, fan-out cost known | Scaling table (1 to 8 loops) and pub/sub fan-out table, with their limits stated | `docs/PERFORMANCE.md` |
| TLS is hardened | testssl.sh Overall Grade A+, Autobahn over wss 0 FAILED | ADR-0019 amendment 6 |
| Resource limits hold | Per-IP caps and rate (ADR-0021), descriptor budget (ADR-0022), bounded every queue and table, decompression bombs refused (ADR-0020), admin port bounded (ADR-0023), broker bounds (ADR-0024) | `docs/SECURITY_DESIGN.md` threat table, tests named there |
| Security reviewed | Three independent read-only reviews: M9 slices (PR #42), the second review (PR #43), the M10 slices (PR #50); findings fixed or recorded with a decision | section 3, the ADR amendments |
| The C ABI is stable and checked | Exported symbols list enforced by the linker, header matches symbols, a C99-only consumer test, all in `ctest` | `capi_abi_symbols`, `capi_header_matches_symbols`, `capi_consumer` |
| Observable | `/metrics`, `/healthz`, `/readyz` on a separate listener, closed set of counters | `docs/OPERATIONS.md` section 3 |

## 3. Known limitations (stated, not hidden)

Each is either a documented decision or a follow-up with an owner decision pending.

- **Linux only** (epoll). On macOS and Windows develop with Docker Desktop or WSL2 and run the worker next to the daemon ([DEPLOYMENT.md](DEPLOYMENT.md) section 11). Planned for a future 1.x release, not 1.0: TCP IPC (a worker on the host talking to `wsrvd` in a container) and a native macOS server (kqueue), then Windows (M13). The PHP and JavaScript clients are tested on Apple Silicon macOS in CI.
- **Rooms from PHP work through the sidecar only.** `wsrvd` with `--broker=true` gives a PHP worker `Connection::subscribe` and `Worker::publish`, and any process can publish with `Wsrv\Publisher` over the HTTP endpoint (ADR-0028). The C ABI (`wsrv_publish` / `wsrv_subscribe`) and so the PHP-FFI embedded mode do not have rooms yet (ADR-0027 slice 2, planned as ABI 0.5).
- **permessage-deflate multiplies broadcast cost** per subscriber (ADR-0024 A2). Leave compression off for broadcast-heavy deployments, or lower `max_payload_bytes`.
- **Broker bounds are global**, not per user (ADR-0024 A4): an application serving strangers adds its own quota.
- **The admin listener has no authentication** and no `Host` check (SECURITY_DESIGN T21); keep it on loopback or behind a network policy.
- **Metrics are counters and gauges only.** The histograms (`handshake_seconds`, `loop_lag_seconds`, `outbound_queue_bytes`) in OPERATIONS section 3 are planned; there is no loop-lag watchdog yet (SECURITY_DESIGN T16). **Decision: post-1.0**; the handlers run on the host thread, so a slow handler cannot stall an I/O loop.
- **From the second security review, still open (owner delegated the decisions to the assistant on 2026-11-22; items 1, 2, 3 and 5 of that review were fixed in #43):** (a) `SessionHandle` sends copy whole messages into posted closures, bounded only by the task-queue size: **built** (2026-11-23, ADR-0032): a per-loop byte budget `max_pending_send_bytes` (default 64 MiB), `BufferFull` when used up, `InvalidArgument` for a message larger than the whole budget. (b) No global buffered-bytes budget across connections (about 3 MiB per connection worst case with deflate, OPERATIONS 1.1): **accepted for 1.0 with the sizing formula documented; a budget key is post-1.0**. (c) The zlib state of a connection lives for the connection once used: **accepted**, it is bounded by the window setting.
- **`IpLimits::sweep`** scans the whole table under its mutex on the accept path when the table is full (low risk at the default rates). **Accepted; amortise it when a benchmark or a report shows it matters.**
- **Per-IP limits can be switched off** with `0`. **Decision (2026-11-22): confirmed as the one exception** to "no unlimited value": behind a reverse proxy or NAT every user is one address, and the defaults would refuse real users. They are on by default, and the C keys of ADR-0016 A1 refuse `0` (no "off" there).
- **PHP binding**: the sidecar client and worker (with rooms and the HTTP `Publisher`), and the embedded FFI server (`Wsrv\Embedded\Server`, ADR-0025, with the `authenticate` handshake hook, no rooms yet). Examples: `examples/chat`, the Docker quickstart, and `docs/LARAVEL.md` (tried by hand against Laravel 13, not in CI). Not done: loop adapters (ReactPHP, Revolt) and a Laravel Broadcasting driver. Published: `wsrv/client` on Packagist, `wsrvd` images on GHCR, checksummed binaries with build attestation on GitHub Releases (ADR-0026; `0.1.0-rc.2` was verified end to end from those artifacts on 2026-11-23). The PHP tests run on PHP 8.2 (the floor) and the latest release in `.github/workflows/php.yml`. The C consumer and the PHP binding pass Autobahn (run by hand with `tests/autobahn/run.sh --capi` and `--php`, not in CI: the 1.5 GB suite image is not part of the CI cost design).
- **Single-machine numbers** (PERFORMANCE.md): comparisons between loop counts, not capacity planning. There is no CI speed gate, by decision (PERFORMANCE.md "Regression gates").

## 4. Release checklist

Tick only with evidence. "Owner" items need a decision that is not the assistant's.

Engineering
- [x] All CI jobs green on main (gcc dev and release with LTO, clang with libc++ and TSan and fuzz, clang-tidy)
- [x] Autobahn: 0 FAILED (517 cases), plain and wss; re-run on the 1.0.0 sources through the C++ server, the C consumer and PHP, plain and wss (2026-11-26, `tests/autobahn/EXCLUDED.md`)
- [x] Fuzzers run clean in CI; corpus committed
- [x] Soak run of one hour passes (`wsrv_load --churn`)
- [x] Chaos soak of two hours passes: restarts, `kill -9` and network cuts under 200 reconnecting clients, release image of `c460f79` (2026-11-25; `tests/soak/README.md`)
- [x] Benchmarks published with their limits (`docs/PERFORMANCE.md`; against Workerman 5, Swoole and Reverb, one and four CPUs: `docs/BENCHMARKS.md`)
- [x] Security review of the last milestone done, findings fixed or recorded
- [x] The default limits of REQUIREMENTS section 4 exist in the configuration and are enforced (index: PROTOCOL_EDGE_CASES section 10)
- [x] A row-by-row audit of REQUIREMENTS section 4 against its code and tests (2026-11-26, [LIMITS_AUDIT.md](LIMITS_AUDIT.md)): all 19 limits are enforced with the documented default and answer; three defaults had no test (now asserted); two rows describe the design differently from the code (the global connection cap closes without a 503, the read buffer is one shared 16 KiB per loop): the code was the safer side, so the requirements text now follows it (owner, 2026-11-26)
- [x] Operations guide covers config, metrics, shutdown, hardening, runbook (OPERATIONS)
- [x] ABI checks in CI
- [x] Coverage re-measured against the NFR-M4 thresholds (2026-11-26, develop at `7aad5fd`, GCC 14 `coverage` preset with TLS and deflate on, 1425 tests, all passing): **92.3 % of the lines and 85.4 % of the branches of `src/`, every module above its threshold.** `protocol` 96.5 % / 90.5 %, `http` 96.1 % / 91.5 %, `base` 97.4 % / 89.0 %, `crypto` 99.4 % / 100 % (needed 90 / 85); `capi` 90.3 %, `server` 90.0 %, `net` 89.8 %, `tls` 91.3 %, `ipc` 98.7 %, `extensions` 81.8 % lines (needed 75). Reproduce with `tools/coverage.sh`. Caveats: only `src/` is measured (not `apps/wsrvd`, the bindings or the examples); branches that exist only for exceptions or can never be taken are excluded; gcov's non-atomic counters can go negative in threaded tests and are read as zero, which only understates; coverage is checked on demand, it is not a CI gate
- [x] Autobahn through the C consumer (`examples/echo_c`, plain and wss, 517 cases, 0 FAILED; `tests/autobahn/EXCLUDED.md`)
- [x] Autobahn through PHP (`bindings/php/examples/embedded_echo.php`, plain and wss, 517 cases, 0 FAILED; `tests/autobahn/EXCLUDED.md`)
- [x] The open items of section 3 either fixed or accepted (decisions of 2026-11-22 above; the send byte budget was built, ADR-0032, and the permessage-deflate answers are in ADR-0020 A2)
- [x] ADR-0020 A2 items 1 and 3 implemented with their tests (EC-D08, EC-D09), and a test for the release of the send scratch buffer (EC-D05)

Repository and launch (M14, listed here so nothing is forgotten)
- [x] Owner: final project name (`wsrv`, confirmed 2026-11-22; Composer name `wsrv/client`, found free on Packagist) and license confirmed
- [x] `NOTICE`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md` added (repository root)
- [ ] Owner: review of the git history (author e-mail addresses) and a full-history secret scan before the repository goes public
- [ ] Owner: branch protection on `main`; secret scanning, push protection, private vulnerability reporting enabled
  - Recommendation (2026-11-22): block force pushes and branch deletion on `main`; do **not** require status checks while `ci.yml` uses `paths-ignore`, because a docs-only PR gets no status and would stay blocked forever. Required checks are an M14 decision. Not applied: a repository setting, and a private repository may need a paid plan for it.
- [x] Signed release artifacts and an SBOM (ADR-0033), verified on `1.0.0-rc.1` (2026-11-26): `SHA256SUMS` matches all five files; `gh attestation verify` passes for the four binaries, the SBOM and the image (and fails on a file with one byte added); `cosign verify` (cosign 3) passes for the image with the identity `release.yml@refs/tags/v1.0.0-rc.1`; the SBOM is SPDX 2.3 with 93 packages; the multi-arch image (30 MB, non-root, `latest` not set) starts, answers a handshake (101 for the allowed origin, 403 for another) and becomes healthy on x86-64. The arm64 image could not be run on the development machine (no emulation). **`1.0.0-rc.2`** (same day, with the image-start step of ADR-0033 A1) closed that: both images were started and went healthy on their native runners (amd64 and arm64) before being pushed; the image was also copied to Docker Hub (`dimathjayasinghe/wsrvd:1.0.0-rc.2`, public, anonymous pull answers 200) with the same digest as on GHCR, `cosign verify` (cosign 3) passes there too, the five release files match `SHA256SUMS` and carry attestations, and neither registry has a `latest` tag.
- [x] Versioning policy stated: README "Stability" (IPC protocol 0.3 frozen for 1.x, C ABI 1.0 with ADR-0016 A4, PHP package follows semantic versioning; the C++ headers under `src/` are not promised)

## 5. Go / no-go

**Release candidate for evaluation: yes.** The server core, TLS, deflate, limits, broker, metrics, C ABI and sidecar are built, tested under every sanitizer, fuzzed and reviewed three times; the claims in section 2 can be checked by running the named commands.

**Public 1.0: not yet.** What is left: the coverage figures re-measured (section 4), and the owner's decisions (the history review, branch protection, and the go for `v1.0.0` and the `latest` tag). After the final release: the Docker Hub overview text (`docker/HUB.md` pulls `latest`, which exists only then) and switching the install lines from the release candidate (the checklist line above). The three open review items of the second security review are closed (section 3). This document does not change repository visibility or publish a final release; that is the owner's call.

## 6. Going public (checklist, 2026-11-22)

Done in the repository: `LICENSE`, `NOTICE`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md`, `GOVERNANCE.md`, `SECURITY.md` (reports through GitHub's private advisory), `CONTRIBUTING.md` with a "Where to start" list, issue and PR templates (blank issues off, security link), README status banner, scan of the history and the tree for secrets and personal data (patterns only; no key, token or certificate found), branch protection on `main` (no force push, no deletion), repository-local commit email set to the GitHub noreply address.

Owner decisions that cannot be made for the owner:
- [x] **The project name**: `wsrv`, Composer package `wsrv/client` (owner, 2026-11-22; Packagist showed no `wsrv/` vendor).
- [ ] **Commit history.** All commits carry the owner's personal email (`git log --format=%ae`). Rewriting history would break every PR and merge link; the alternative is to accept it. New commits use the noreply address. In GitHub, also tick "Block command line pushes that expose my email" (account settings, Emails).
- [x] **Flip the visibility**: done, the repository is public (checked with `gh repo view`, 2026-11-25). Announcing is a separate step that waits for the final `v1.0.0`.

Right after the flip (not available while the repository is private):
- [x] Settings, Code security: private vulnerability reporting, secret scanning, push protection and Dependabot alerts are on (checked with `gh api`, 2026-11-26). Dependabot security updates (automatic fix PRs) are off: optional, the owner's call.
- [x] Private vulnerability reporting is enabled, so the Security tab offers "Report a vulnerability" (SECURITY.md and the issue template link depend on it).
- [x] Pushed `v1.0.0-rc.1`: `release.yml` ran with the arm runner, the attestation and the signature (see the signed-artifact line above), and Packagist lists `wsrv/client` `v1.0.0-rc.1`.
- [x] The GHCR package `wsrvd` is public (an anonymous manifest fetch of `1.0.0-rc.1` answers 200).
- [ ] **When `1.0.0` is tagged**, switch the install lines from the release candidate to the final version: `README.md` (status line, the `wsrvd:` image and the `composer require` row), `docs/LARAVEL.md` and `bindings/php/README.md` (`composer require wsrv/client:^1.0@rc` becomes plain `composer require wsrv/client`), `examples/docker/compose.yaml` and `examples/docker/worker/Dockerfile` (`1.0.0-rc.1` becomes `1.0.0`; the Dockerfile may then use `^1.0`), and check `deploy/kubernetes/wsrv.yaml` (already `1.0.0`).
- [ ] Optionally create the first "good first issue" labels and issues from CONTRIBUTING.md's list.
