# Evidence: what has been checked, and how to check it yourself

A claim you cannot check is an adjective. This page lists what wsrv claims about being correct, safe and fast, the command that checks each claim, what the last run showed, and what that run does **not** show. The sources of truth are linked in each row; if a number here disagrees with them, they win ([docs/RELEASE.md](RELEASE.md) keeps the full record for 1.0).

All results are public: every change runs the CI jobs listed below at <https://github.com/DimathJayasinghe/wsrv/actions>, and what CI does not run is marked "by hand".

To run the build-and-test commands you need Docker (or a Linux machine with GCC 14 or Clang 18 with libc++, and CMake 3.25+):

```sh
make image && make shell       # once: a dev container with the compilers, the sanitizers and the tools
# inside the container, from the repository root:
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev --output-on-failure
```

## Correctness

| Claim | Check it | Last result | Not shown |
|---|---|---|---|
| **It speaks RFC 6455 and 7692 correctly.** | `tests/autobahn/run.sh` (add `--wss` for TLS, `--capi` for the pure-C server, `--php` for the PHP binding). Needs Docker; run by hand before a release, not in CI. | Autobahn\|Testsuite sections 1-7, 9, 10, 12, 13: 517 cases, 0 failed, plain and over `wss://`, re-run on the 1.0.0 sources through the C++ server, the C consumer and the PHP binding (2026-11-26). The excluded cases and why: [tests/autobahn/EXCLUDED.md](../tests/autobahn/EXCLUDED.md). | Autobahn tests the protocol. It says nothing about your application's handler. |
| **Every edge case has a test.** | [PROTOCOL_EDGE_CASES.md](PROTOCOL_EDGE_CASES.md) maps each row to the tests that cover it; `ctest --preset dev` runs them. | The whole suite passes under ASan and UBSan, and again under TSan, in CI on every change. | A row is only as good as the test behind it; the index shows which rows are tests and which are design decisions. |
| **The default limits are enforced.** | [LIMITS_AUDIT.md](LIMITS_AUDIT.md) checks each of the 20 default limits of REQUIREMENTS section 4 against the code and a test. | All 20 enforced with the documented default and answer (19 audited for 1.0, the global buffer budget of ADR-0040 added in 1.1); three defaults had no test and now have one (2026-11-26). | Limits are per process and per address; see "Broker bounds are global" in [RELEASE.md](RELEASE.md) section 3. |
| **The tests exercise the code.** | `tools/coverage.sh` in the dev container (it builds a separate coverage tree; allow a long compile). | `src/`: 92.3 % of lines, 85.4 % of branches, every module above its threshold (2026-11-26, 1.0). | Coverage says the lines ran, not that they were checked well. |

## Safety

| Claim | Check it | Last result | Not shown |
|---|---|---|---|
| **No memory errors and no undefined behaviour.** | `ctest --preset dev` (the dev preset builds with ASan and UBSan). In CI on every change. | Clean. | Sanitizers find what the tests reach. |
| **No data races.** | `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan`. In CI on every change. | Clean, including the multi-thread stress tests and the broker tests. | Same: what the tests reach. |
| **Hostile input does not crash or leak.** | 14 libFuzzer targets (frame parser, handshake, HTTP head, publish request, admin request, UTF-8, message pipeline, session, deflate, IPC frame, handshake blob, TLS server, SHA-1, Base64). CI replays every committed corpus and runs each target for 5 seconds on every change. To run one yourself: (inside `make shell`; on a bare host you need Clang 18 with libc++ and CI's `-DCMAKE_CXX_COMPILER=clang++-18`) `cmake --preset fuzz && cmake --build --preset fuzz --target fuzz_frame_parser && mkdir -p /tmp/fz && build/fuzz/tests/fuzz/fuzz_frame_parser /tmp/fz tests/fuzz/corpus/frame_parser -max_total_time=60` | Clean. Each target asserts a property (for example: the admin listener never renders `/metrics` for a request that does not contain the token). | Five seconds per change is a smoke test; longer runs are done by hand. There is no continuous fuzzing service yet. |
| **Static checks pass.** | `cmake --build --preset dev --target format-check check-banned check-includes header-check`; clang-tidy with warnings as errors (`cmake --preset tidy && cmake --build --preset tidy`) runs in CI on the production code (`src/`, `apps/`, the examples and the bench tools); tests and fuzz harnesses are tidied only locally, with the `tidy-all` preset. | Clean. The banned-function list (`sprintf`, `strcpy`, `atoi`, ...) and the module-boundary rules (`protocol/` never includes OS headers) are enforced mechanically. | |
| **The threat model has controls and tests.** | [SECURITY_DESIGN.md](SECURITY_DESIGN.md): each threat, its control and the tests that pin it. | 24 rows. | **There has been no external security audit.** The reviews done during development are recorded in [RELEASE.md](RELEASE.md) section 3 with their findings and decisions; they are not a third-party audit. |
| **TLS is hardened.** | `tests/tls/run_testssl.sh` (by hand; it needs the third-party `testssl.sh`, which the script tells you how to fetch and asks you to read first) | Overall grade A+, score 96: TLS 1.2 and 1.3 only, forward-secret AEAD suites only, no resumption (ADR-0019 amendment 6). Autobahn over `wss://` also passes. | TLS is OpenSSL's; the grade is for the configuration wsrv sets, on one date and one `testssl.sh` version. |
| **Releases are what the source says.** | `cosign verify dimathjayasinghe/wsrvd:X.Y.Z --certificate-identity-regexp 'https://github.com/DimathJayasinghe/wsrv/\.github/workflows/release\.yml@refs/tags/v.*' --certificate-oidc-issuer https://token.actions.githubusercontent.com` (cosign 3 or newer) | Images are scanned before they are pushed, signed keyless and carry build provenance; each release has an SBOM and the binaries are checksummed. | A signature proves where the image was built, not that the code is free of bugs. |

## Staying up

| Claim | Check it | Last result | Not shown |
|---|---|---|---|
| **No leaks over time.** | `wsrv_load --threads 4 --clients 4 --connections 8 --seconds 3600 --churn --sample 60` (see [PERFORMANCE.md](PERFORMANCE.md) "Soak") | 1 hour, 32 churning connections: open descriptors stayed between 78 and 86, resident memory settled at 2.4 MiB and stayed flat (2026-10-30, an earlier build). | One hour, small messages, one machine. |
| **It recovers from restarts, kills and network cuts.** | `python3 tests/soak/soak.py --image <wsrvd image> --hours 2 --clients 200` (needs Docker; [tests/soak/README.md](../tests/soak/README.md)) | 2 hours on the release image, 200 reconnecting JavaScript clients, 162 faults: 69 daemon restarts (37 graceful, 32 `kill -9`), 63 worker restarts and kills, 30 network cuts of 3 to 10 s. After every fault all 200 clients were connected again (26,400 reconnects in total); descriptors flat; daemon memory flat (4.0 MB at warm-up, 3.8 MB at the end); the daemon never crashed (2026-11-25). The run found and fixed a client bug on the way. | By hand, not in CI. The clients are this project's JavaScript client; yours may behave differently. |

## Speed

[BENCHMARKS.md](BENCHMARKS.md) compares wsrv with Workerman 5, Swoole and Laravel Reverb on one machine, with every number, the noise between runs and the harness. To run it on your hardware (needs Docker; it builds everything it runs):

```sh
docker build -f docker/wsrvd.Dockerfile --build-arg REVISION=$(git rev-parse --short HEAD) -t wsrvd:local .
BENCH_MEMORY=2g python3 bench/compare/run.py --servers wsrv,workerman,swoole,reverb --repeats 3 --label my-run
```

What it showed (2026-11, one laptop): **with four CPUs wsrv is ahead on echo** (about 1.7 to 1.9 times Workerman and Swoole with 512 connections) and reaches the last of 5,000 subscribers of a broadcast in about half the time; **on one CPU it is behind on echo** by about 10 to 25 percent, because the daemon and the PHP worker are two processes and every message crosses a Unix socket twice; memory per idle connection is a tie with the single-process servers (8 to 9 KiB) and Reverb uses about 2.5 times as much.

What it does not show: TLS, permessage-deflate, authentication, a database behind your handlers, a WAN, or Workerman and Swoole running several worker processes. One laptop under Docker Desktop and a load generator written by this project are a comparison, not a capacity plan, and there is no CI speed gate by decision ([PERFORMANCE.md](PERFORMANCE.md) "Regression gates"). Run your own handler under your own load before you size anything.

## What is not evidenced

Stated so you do not have to find it out:

- **No external audit** of the code, the protocol implementation or the TLS configuration.
- **No users yet at scale.** The project is young; the evidence above is from its own tests and tools.
- **Laravel:** the ticket guide is tried by hand against Laravel 13, not in CI ([LARAVEL.md](LARAVEL.md)). The Echo support planned for 1.2 ([LARAVEL_ECHO.md](LARAVEL_ECHO.md)) has unit tests and a real-daemon test in CI, and was run by hand with the real `pusher-js` and `laravel-echo` in Chrome and against Laravel 13.43; section 9 of that guide lists what was not run (other browsers, Laravel 11 and 12, Echo's `pusher` broadcaster).
- **macOS and Windows:** the server is Linux only. CI runs the PHP client tests on both against a daemon stand-in; the whole path (a host worker, a published port, Docker Desktop) is checked on Linux, not on a real Mac or PC ([DEPLOYMENT.md](DEPLOYMENT.md) section 11).
- **Several nodes:** there is no built-in message backplane; you publish to each daemon ([DEPLOYMENT.md](DEPLOYMENT.md) section 7).
- **Known limitations** with their decisions: [RELEASE.md](RELEASE.md) section 3.

If you find a claim here that a check does not support, that is a bug in this page: please [open an issue](https://github.com/DimathJayasinghe/wsrv/issues).
