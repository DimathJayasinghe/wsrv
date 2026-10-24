# Build & Tooling

## 1. Toolchain

| Tool | Minimum | Notes |
|---|---|---|
| GCC | 13 | Dev machine currently GCC 15.2 |
| Clang | 17 | Needed for MSAN, libFuzzer, clang-tidy |
| CMake | 3.25 | Presets v6 |
| Ninja | 1.11 | Default generator |
| clang-format / clang-tidy | 17+ | Pinned version in CI image |
| cppcheck | 2.13+ | Secondary analysis |
| OpenSSL | 3.0+ | Only with `WSRV_ENABLE_TLS=ON` |
| zlib | 1.3+ | Only with `WSRV_ENABLE_DEFLATE=ON` |
| Docker | any | Autobahn runner |
| Python | 3.11 | Helper scripts only (not runtime) |

> Local machine check (2026-10-24): host has `g++ 15.2`, `OpenSSL 3.5.5`, Docker 29.9 (Docker Desktop); it lacks `cmake`, `ninja`, `clang*`. **All development happens in the Docker dev container (§10)**, which provides the full toolchain, so nothing needs installing on the host.

### 1.1 Standard-library rule (verified in the dev container, 2026-10-24)

| Compiler | Standard library | Why |
|---|---|---|
| GCC 13+ | libstdc++ | default; `std::expected`, `std::span` work with `-std=c++23` |
| Clang 17/18 | **libc++ (`-stdlib=libc++`)** | Clang ≤ 18 + libstdc++ lacks `std::expected` (Clang reports `__cpp_concepts` below the level libstdc++ requires). libc++ provides it and is also what MSAN needs (instrumented build) |

The CMake presets for every Clang configuration (`tsan`, `msan`, `fuzz`, tidy) therefore pass `-stdlib=libc++`; the GCC presets do not. A configure-time check compiles a tiny `std::expected` program and fails fast with a clear message if the toolchain can't.

Verified: GCC 14.2 and Clang 18.1.3 (libc++) both compile C++23 `std::expected`/`std::span` and link with `-fsanitize=address,undefined` and `-fsanitize=thread`.

Dev container versions: GCC 14.2, Clang/LLVM 18.1.3, CMake 3.28.3, Ninja 1.11.1, cppcheck 2.13, IWYU 0.21, OpenSSL 3.0.13, PHP 8.3.6 (+FFI).

## 2. Repository layout

```
CMakeLists.txt  CMakePresets.json  .clang-format  .clang-tidy  .editorconfig  .gitignore
cmake/            toolchain helpers, warnings.cmake, sanitizers.cmake, hardening.cmake, banned_functions.cmake
include/wsrv/     public headers
src/<module>/     private; one CMake target per module (wsrv_base, wsrv_crypto, wsrv_http, wsrv_protocol, wsrv_net, wsrv_server)
tests/<module>/   mirrors src; tests/support, tests/contract, tests/integration, tests/fuzz, tests/autobahn, tests/bench
examples/         echo, chat
docs/             documentation and ADRs
tools/            scripts (lint, include-graph check, banned-function scan)
.github/          workflows, PULL_REQUEST_TEMPLATE.md, ISSUE_TEMPLATE/
```

## 3. CMake rules

- One target per module. Linking declares the allowed-dependency graph from ARCHITECTURE §2.1 and the `module_graph` test diffs it against `tests/module_graph.expected` (it sees PUBLIC, INTERFACE and PRIVATE links). All modules share the `src/` include root, so a stray `#include` is **not** a build error; `tools/check_includes.py` (`check-includes`) is the primary guard and also rejects OS headers in the sans-I/O modules. (Per-module include roots would make it a hard error; revisit if the checker proves insufficient.)
- `wsrv::wsrv` alias is the only installable library target; `install()` + `find_package(wsrv)` config files; semantic versioning.
- No globbing for sources (explicit lists) so CI detects missing files.
- Options (all default OFF unless stated): `WSRV_BUILD_TESTS` (ON for top-level), `WSRV_BUILD_EXAMPLES`, `WSRV_ENABLE_TLS`, `WSRV_ENABLE_DEFLATE`, `WSRV_BUILD_FUZZERS`, `WSRV_BUILD_BENCH`, `WSRV_WARNINGS_AS_ERRORS` (ON), `WSRV_SANITIZER` (none|asan-ubsan|tsan|msan).
- Out-of-source builds only; reproducible: no timestamps, `-ffile-prefix-map`.
- Dependencies for tests fetched via `FetchContent` with pinned tag **and** hash, or vendored under `third_party/` with license file; never floating `main`.

## 4. Presets (`CMakePresets.json`)

| Preset | Purpose |
|---|---|
| `dev` | Debug, ASAN+UBSAN, tests, warnings as errors |
| `release` | `-O2 -DNDEBUG`, LTO, hardening flags |
| `relwithdebinfo` | Profiling |
| `tsan` | Thread sanitizer |
| `msan` | Clang + MSAN |
| `fuzz` | Clang, libFuzzer, ASAN+UBSAN |
| `coverage` | gcov/llvm-cov |
| `tidy` | Run clang-tidy during build |
| `bench` | Release + benchmarks |

Usage: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`.

## 5. Lint & format gates

- `.clang-format` committed; CI runs `clang-format --dry-run -Werror`.
- `.clang-tidy` committed with `WarningsAsErrors: '*'` (suppress only with `// NOLINT(check): reason`; reasons reviewed).
- `tools/check_banned_functions.sh` greps for `strcpy|sprintf|gets|strtok|atoi|rand\(|system\(|popen`.
- `tools/check_includes.py` enforces module dependency graph on `#include` lines.
- Header self-containment target compiles each header alone.
- `pre-commit` hooks (format, trailing whitespace, banned functions, markdown links) — optional locally, enforced in CI.
- `.editorconfig`: LF, UTF-8, final newline, 4 spaces (2 for YAML/JSON/CMake).

Implemented gate targets (all run in CI): `cmake --build --preset dev --target format-check | check-banned | check-includes | header-check`; `tools/format.sh fix` rewrites formatting; `cmake --preset tidy && cmake --build --preset tidy` runs clang-tidy on every first-party compile. The tools themselves are tested (`ctest -L tools`) against fixtures in `tests/tools/fixtures/`. `module_graph` (ctest) diffs the real CMake link graph against `tests/module_graph.expected`. Sanitizer self-tests: `ctest -L sanitizer-selftest`.

### 5.1 Fuzzing (implemented in M1)

```bash
cmake --preset fuzz && cmake --build --preset fuzz            # Clang + libc++, ASan+UBSan, libFuzzer
cd build/fuzz && ctest -L fuzz-regression                      # replay the committed seed/regression corpora
./tests/fuzz/fuzz_utf8 ../../tests/fuzz/corpus/utf8 -max_total_time=60   # real fuzzing; writes crash-* on failure
```

Targets: `fuzz_utf8`, `fuzz_base64`, `fuzz_sha1` (properties asserted inside each harness). Every crashing input must be added to `tests/fuzz/corpus/<target>/` together with its fix. Toolchain traps (documented in `tests/fuzz/CMakeLists.txt`): Ubuntu's libFuzzer runtime is built against libstdc++ while our Clang builds use libc++, and the Clang driver rewrites `-lstdc++` into `-lc++` under `-stdlib=libc++`, so the fuzz executables link `-l:libstdc++.so.6` with `--no-as-needed`. CI does not run the fuzzers yet (cost); the corpus replay is cheap and will be added once the number of targets justifies a job.

## 6. CI pipeline (GitHub Actions)

### 6.1 Implemented (M0) and its cost design

| Workflow | Runs when | Jobs | Measured |
|---|---|---|---|
| `ci.yml` | push to `main` and PRs, **except** docs-only changes (`docs/**`, `*.md`, `LICENSE`) | `gcc`: quality gates + `dev` + `release`; `clang`: `dev-clang` + `tsan` + `tidy` | ~2.3 billed minutes |
| `image.yml` | only when `docker/**`, `docker-compose.yml` or the dev-env action change | builds the dev image and runs a build + test inside it | ~1.5 min, rarely |

Why it looks like this: the first version built the Docker dev image in each of five jobs and billed ~19 minutes, ~16 of them image builds, for 16–35 s of real work per job. Now CI uses the runner's preinstalled GCC/Clang/CMake/Ninja (only libc++ and clang-tidy are installed), runs few jobs (each job pays runner start-up), skips changes that cannot affect the build, and rebuilds the image only when it can have changed. Trade-off: CI is not byte-identical to the dev container (runner GCC 13/14 vs container GCC 14.2); the CMake version floors and the `image.yml` check bound that risk. Reproduce a CI job locally with `tools/ci-run.sh '<command>'` (runs inside the dev image).

Required checks: do **not** make `gcc`/`clang` required status checks while `paths-ignore` is active — a docs-only PR would wait forever for a check that never starts (use an always-running job with an internal path filter first). `cancel-in-progress` applies to PRs only, so a push to `main` is never cancelled. Branch protection is not enabled yet (owner decision; GitHub Free private repositories cannot enforce it).

Further savings when builds grow: cache ccache across runs (`actions/cache` keyed on preset + compiler), split a slow job only if wall-clock matters (billed minutes stay equal). Actions are pinned to commit SHAs and the token is read-only.

### 6.2 Planned pipeline (later milestones)

1. **lint** — format, tidy (changed files), banned functions, include graph, markdown/link check, license headers.
2. **build-test matrix** — {gcc-13, gcc-latest, clang-17+} × {debug+asan-ubsan, release}.
3. **tsan** — threaded suites.
4. **fuzz-smoke** — 60 s per target.
5. **autobahn** — docker, quick subset on PR / full on main.
6. **coverage** — upload, enforce thresholds.
7. **nightly** — long fuzz, MSAN, cppcheck, `-fanalyzer`, benchmarks, soak (weekly).
8. **release** — tag → build artifacts, SBOM (CycloneDX), checksums, signature, changelog.

Caching: ccache + CMake build dir keyed by compiler/preset. CI images pinned by digest.

## 7. Dependency policy

1. Prefer the standard library and in-tree code. A dependency must save > ~500 lines of non-trivial code or be security-critical (OpenSSL, zlib) and not sensible to write.
2. Each dependency: ADR, license compatibility check, pinned version + hash, upgrade owner, listed in `THIRD_PARTY.md` and the SBOM.
3. Core library (`wsrv_base`…`wsrv_protocol`) has **zero** third-party dependencies.
4. Optional deps isolated in their own targets (`wsrv_tls`, `wsrv_deflate`) behind interfaces.
5. Monthly dependency review; security advisories trigger patch within 7 days.

## 8. Versioning & releases

- SemVer. Public API = `include/wsrv/`. Breaking change ⇒ major bump + migration notes.
- `CHANGELOG.md` (Keep a Changelog). Release branches only for backports.
- Release checklist (`docs/RELEASE_CHECKLIST.md`, created at M10): all gates green, Autobahn report attached, security review done, docs updated, tag signed.

## 9. Local developer workflow

```bash
cmake --preset dev && cmake --build --preset dev -j
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check tidy
./build/dev/examples/echo --port 9001      # then: docker run autobahn fuzzingclient
```

## 10. Docker development environment

**Why:** one reproducible toolchain for every contributor and for CI (no "works on my machine"), full sanitizer/fuzz/analysis tooling without installing anything on the host, and passwordless `sudo` *inside the container only* so extra packages can be added without touching the host.

| File | Purpose |
|---|---|
| `docker/Dockerfile` | Stage `dev`: Ubuntu 24.04, GCC 14, Clang/LLVM 18 (+format, tidy, libc++, sanitizer runtimes), CMake, Ninja, ccache, gdb, valgrind, perf, cppcheck, gcovr, IWYU, OpenSSL + zlib dev, PHP CLI + FFI (for M12). Non-root user `dev` created with the host UID/GID, passwordless sudo |
| `docker-compose.yml` | Service `dev` (bind-mounts the repo at its own host path (`PROJECT_DIR`, set by the Makefile), ccache volume, `SYS_PTRACE` for gdb/LeakSanitizer, high `nofile` limit, ports 8080/9100); profile `conformance` runs Autobahn |
| `Makefile` | Thin wrappers: `make image`, `make shell`, `make root-shell`, `make status`, `make down` |
| `.dockerignore` | Keeps the build context small |

Usage:

```bash
make image        # once (and when Dockerfile changes)
make shell        # shell as user 'dev' in the same path as the host repo; `sudo apt install …` works
make status       # print toolchain versions
```

Rules:
- The `dev` stage is **never** shipped. A separate minimal `runtime` stage (non-root, no shell, no sudo, read-only rootfs; see OPERATIONS §5) is added at M6.
- `SYS_PTRACE` and `seccomp=unconfined` exist only in the dev compose service so debuggers and LeakSanitizer work.
- CI uses the same Dockerfile; base image pinned by digest there.
- Version floors (GCC ≥ 13, Clang ≥ 17) are asserted by CMake at configure time (M0), so a drifted image fails loudly.
- **Docker Desktop bind-mount quirk (found 2026-10-24):** a host file that is *replaced by rename* (what many editors, `git checkout`/`git pull`, and "atomic save" do) can stay stale inside the container — the container keeps seeing the old content, so the build silently uses old code. In-place edits and brand-new files are fine. Symptoms: "undefined reference" or tests ignoring a fix you can see on the host. Fixes: `docker compose restart dev` (refreshes everything), or `rm` the file before recreating it, or edit in place. Safest habit: after `git pull`/branch switch, run `make restart`. Native Linux Docker Engine (no VM) does not have this problem.
- Host-network note: the build needs registry access (`registry-1.docker.io`, `production.cloudfront.docker.com`). If pulls fail with a DNS error, fix the host resolver (e.g. `sudo resolvectl dns <iface> 1.1.1.1 8.8.8.8`) or use another network.
