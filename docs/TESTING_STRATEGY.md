# Testing Strategy

Goal: make protocol bugs and memory-safety bugs structurally hard to ship. Protocol code is sans-I/O precisely so most testing is fast, deterministic, and sleep-free.

## 1. Pyramid

| Level | Tooling | Scope | Share | Runs |
|---|---|---|---|---|
| Unit | GoogleTest (+GMock for interfaces) | One class; fake collaborators (`FakeClock`, `FakeTransport`, `FakePoller`) | ~70 % | Every commit |
| Property / differential | GoogleTest + small in-tree generator (or RapidCheck) | Split-at-every-offset, round-trips, UTF-8 vs reference, masker scalar vs SIMD | ~10 % | Every commit |
| Contract | Typed test suites | Every `ITransport`/`IPoller`/`IHandshakeRule` impl passes same suite | ~5 % | Every commit |
| Integration | Real loopback sockets, in-process server | Handshake→echo→close, limits, timeouts, backpressure, shutdown, fault injection | ~10 % | Every PR |
| Conformance | Autobahn|Testsuite (docker) | Whole RFC 6455/7692 behaviour over ws and wss | — | Every PR to main + nightly |
| Fuzz | libFuzzer (+AFL++ nightly) with ASAN/UBSAN | Parsers and `ProtocolSession` | — | 60 s/PR, ≥ 1 h nightly |
| Soak / chaos | Custom load generator | 24 h churn, random disconnects, slow peers | — | Weekly / pre-release |
| Performance | Google Benchmark + custom load tool | Micro (parser, masker, UTF-8) and macro (echo, fan-out) | — | Nightly, regression gate |

## 2. Rules

1. **No sleeps, no real time** in unit tests: inject `IClock`; drive `tick(now)`.
2. **No flaky tests.** A flaky test is a P1 bug: quarantine within a day, fix within a week, never retry-until-green.
3. **Arrange–Act–Assert**, one behaviour per test, names `Suite.Behaviour_Condition_Expected`.
4. **Test through public interfaces**; peeking at privates means the design needs a seam.
5. **Every EC-row has a test** (see PROTOCOL_EDGE_CASES.md); tests cite the row ID.
6. **Regression first**: bug ⇒ failing test ⇒ fix. Fuzz crashers are added to `tests/fuzz/corpus/regression/`.
7. **Deterministic randomness**: seeds logged and reproducible (`--seed`).
8. **Tests are production code**: reviewed, formatted, linted; helpers live in `tests/support/`.

## 3. Fakes & seams

| Seam | Fake | Purpose |
|---|---|---|
| `IClock` | `FakeClock` (manual advance) | Timeouts, heartbeat, rate limiting |
| `ITransport` | `FakeTransport` (scripted chunking, `WouldBlock`, errors, partial writes) | Connection tests without sockets |
| `IPoller` | `FakePoller` | Loop logic |
| `ILogger` / `IMetrics` | `RecordingLogger` / `RecordingMetrics` | Assert on logs/metrics |
| Syscall layer | Thin `Syscalls` interface with fault injection (`accept`→`EMFILE`, `write`→`EPIPE`) | EC-N tests |
| `IRandom` | `FakeRandom` | Deterministic IDs |

## 4. Protocol test patterns

- **Chunking harness:** `for each split point (and each 1-byte split) feed(bytes) ⇒ identical events/output`. Applies to handshake, frames, UTF-8.
- **Golden scripts:** hex-encoded client→server byte scripts with expected server output (`tests/protocol/golden/*.txt`), human-readable, diffable.
- **Round-trip property:** `parse(serialize(frame)) == frame` for random frames respecting limits.
- **Model-based test:** simple reference implementation of message assembly; compare to optimised version over random op sequences.
- **State-machine matrix:** for each `(state, input)` assert the transition table (illegal ⇒ defined error).

## 5. Fuzzing

Targets (`tests/fuzz/`): `http_request`, `frame_parser`, `protocol_session`(structure-aware: sequences of frames with random flags/lengths/fragmentation), `utf8`, `base64`, `deflate_session`, `config_parser`.

- Built with `-fsanitize=fuzzer,address,undefined`; invariants asserted inside harness (e.g. consumed ≤ input, output bounded by limits, no event after Closed).
- Dictionaries for HTTP tokens and WS opcodes; seed corpus from Autobahn traffic captures and golden scripts.
- CI: 60 s per target per PR; nightly 1 h each with corpus minimisation and persistent corpus artifact. Crash ⇒ CI fails, repro attached.
- Optional: OSS-Fuzz onboarding after 1.0.

## 6. Conformance (Autobahn)

- `tests/autobahn/` has `fuzzingclient.json` (cases `*`, excludes documented in `EXCLUDED.md` with RFC justification) and a runner script (docker).
- Gate: **no `FAILED`**; `NON-STRICT` allowed only if listed; `INFORMATIONAL` reviewed. Report HTML archived by CI.
- Run against plain, TLS, deflate-on, deflate-off builds.

## 7. Sanitizers & dynamic checks

| Preset | Flags | Runs |
|---|---|---|
| `asan-ubsan` | `-fsanitize=address,undefined -fno-sanitize-recover=all` | All tests, every PR |
| `tsan` | `-fsanitize=thread` | Integration + multi-thread tests, every PR |
| `msan` | `-fsanitize=memory` (clang + instrumented libc++) | Nightly (protocol layer) |
| `valgrind` | memcheck | Weekly smoke |
| `libstdc++ assertions` | `-D_GLIBCXX_ASSERTIONS` | Debug and CI release |

Leak and fd-leak checks: integration tests assert open-fd count and RSS stability before/after churn (`/proc/self/fd`).

## 8. Coverage

`gcov/llvm-cov` via `coverage` preset. Thresholds enforced per module (REQUIREMENTS NFR-M4). Coverage is a floor, not a goal: mutation testing (`mull` or `dextool`) quarterly on `protocol/` to validate test strength.

## 9. Performance testing

- Micro: parser, masker (scalar vs SIMD), UTF-8, SHA-1, pool alloc. Stored baselines; CI compares on same runner class; > 5 % regression fails nightly.
- Macro: `tests/bench/load.cpp` (`wsrv_load`: blocking client threads, echo throughput, latency percentiles, churn soak; results and caveats in [PERFORMANCE.md](PERFORMANCE.md)); an epoll-based generator is still wanted for: idle connections (memory/conn), echo throughput/latency histogram (HdrHistogram), broadcast fan-out, handshake rate, connection churn. Environment, kernel params, and commands documented so results reproduce.
- Profile with `perf`, `heaptrack`, flamegraphs; findings recorded in `docs/perf/`.

## 10. CI gating matrix

| Stage | PR | main | nightly | release |
|---|---|---|---|---|
| Build GCC + Clang, warnings=errors | ✔ | ✔ | ✔ | ✔ |
| format / tidy / banned-functions / header check | ✔ | ✔ | ✔ | ✔ |
| Unit + contract + property | ✔ | ✔ | ✔ | ✔ |
| ASAN/UBSAN, TSAN | ✔ | ✔ | ✔ | ✔ |
| Integration | ✔ | ✔ | ✔ | ✔ |
| Fuzz smoke (60 s) | ✔ | ✔ | long | long |
| Autobahn | quick subset | full | full | full (+wss, deflate) |
| Coverage thresholds | ✔ | ✔ | ✔ | ✔ |
| MSAN, `-fanalyzer`, cppcheck | — | — | ✔ | ✔ |
| Benchmarks / regression | — | — | ✔ | ✔ |
| Soak 24 h | — | — | weekly | ✔ |

## 11. Definition of "tested"

A change is tested when: new/changed behaviour has unit tests; edge-case rows updated; error paths exercised (fault injection); fuzz target updated if input-parsing changed; docs reflect reality.
