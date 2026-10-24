# M0 Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CMake build, quality gates, and CI so that every later line of C++ is compiled with strict warnings, sanitizers, lint, and enforced module boundaries from its first commit.

**Architecture:** One CMake target per module (`wsrv_base` … `wsrv_server`) whose `target_link_libraries` encode the allowed-dependency graph; shared helper functions in `cmake/`; presets for every toolchain/sanitizer combination; small scripts in `tools/` that CI runs; everything runs inside the `wsrv-dev` Docker image.

**Tech Stack:** CMake ≥ 3.25 (container: 3.28), Ninja, GCC 14 / Clang 18 (+libc++), GoogleTest (pinned), clang-format/tidy 18, cppcheck, Python 3 (scripts), GitHub Actions.

**Spec:** [PROJECT_PLAN.md](../../PROJECT_PLAN.md) M0, [BUILD_AND_TOOLING.md](../../BUILD_AND_TOOLING.md), [CODING_STANDARDS.md](../../CODING_STANDARDS.md) §1/§10, [ARCHITECTURE.md](../../ARCHITECTURE.md) §2.1, ADR-0001/0002/0005/0008.

## Global Constraints

- C++23, `CMAKE_CXX_EXTENSIONS OFF`; GCC ≥ 13, Clang ≥ 17, CMake ≥ 3.25 (asserted at configure time).
- Clang configurations MUST pass `-stdlib=libc++`; GCC uses libstdc++ (BUILD_AND_TOOLING §1.1).
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror`.
- Release hardening: `-D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection -fPIE -pie -Wl,-z,relro,-z,now`.
- Out-of-source builds only; no source globbing; core modules have zero third-party deps (ADR-0005); test deps pinned by version **and** SHA-256.
- Module dependency graph (ARCHITECTURE §2.1): base←crypto←http←protocol; net←base; server←{base,protocol,net,extensions}; `protocol/ http/ crypto/ base/` never include OS/network headers.
- Presets: `dev`, `release`, `relwithdebinfo`, `tsan`, `msan`, `fuzz`, `coverage`, `tidy`, `bench` (the first four + `coverage` + `tidy` are implemented in M0; `msan`/`fuzz`/`bench` are declared but unused until their milestones).
- Project license Apache-2.0; every new source file starts with an SPDX header.

## Review Focus

- Configure with a too-old compiler or Clang without libc++ → fails immediately with a message naming the fix (not a wall of template errors).
- `cmake -S . -B .` (in-source build) → refused.
- A deliberate `protocol → net` include or `<sys/socket.h>` inside `protocol/` → `check_includes.py` exits non-zero and names file:line.
- A deliberate `strcpy`/`rand()` in `src/` → banned-function check fails; the same word inside a comment or string is also flagged (documented, simple, conservative).
- A header that is not self-contained (uses `std::string` without including it) → header-check target fails the build.
- A deliberate heap overflow in a throwaway test is caught by the `dev` (ASAN) preset — proves sanitizers are actually active, not merely configured.

---

### Task 1: Root build, warnings, toolchain gates, version library

**Files:**
- Create: `CMakeLists.txt`, `cmake/WsrvWarnings.cmake`, `cmake/WsrvHardening.cmake`, `cmake/WsrvToolchainChecks.cmake`, `cmake/WsrvModule.cmake`, `cmake/WsrvTests.cmake`
- Create: `src/base/version.hpp`, `src/base/version.cpp`, `src/base/CMakeLists.txt`
- Create: `tests/base/version_test.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces (CMake): `wsrv_add_module(NAME <n> SOURCES <files...> [DEPS <targets...>])` → static lib `wsrv_<n>`, alias `wsrv::<n>`, include dir `src/`, warnings + hardening applied, `PUBLIC` deps from `DEPS`; if `SOURCES` empty, creates an `INTERFACE` library. `wsrv_add_test(NAME <n> SOURCES <files...> LINK <targets...>)` → executable linked to GoogleTest main, registered with CTest, named `<n>`.
- Produces (C++): `namespace wsrv { [[nodiscard]] std::string_view version() noexcept; }` returning the project version string from `PROJECT_VERSION` (passed as compile definition `WSRV_VERSION`).

- [ ] **Step 1: Write the failing test** `tests/base/version_test.cpp`: `TEST(Version, ReturnsNonEmptyDottedString)` asserting `!wsrv::version().empty()` and that it contains `'.'`; `TEST(Version, MatchesProjectVersion)` asserting `wsrv::version() == WSRV_VERSION`.
- [ ] **Step 2: Run to verify it fails** — `cmake --preset dev` then build: expect configure/compile failure (no `version.hpp`, no preset yet). (Preset arrives in Task 2; for this task use `cmake -S . -B build/t1 -G Ninja -DCMAKE_BUILD_TYPE=Debug`.)
- [ ] **Step 3: Implement** the CMake files and `version.{hpp,cpp}`.
  - `WsrvToolchainChecks.cmake`: `FATAL_ERROR` with a one-line fix hint when (a) `CMAKE_SOURCE_DIR == CMAKE_BINARY_DIR`, (b) GCC < 13 or Clang < 17, (c) a `try_compile` of a tiny `std::expected`+`std::span` C++23 program fails (message: "toolchain lacks std::expected — with Clang use -stdlib=libc++, see docs/BUILD_AND_TOOLING.md §1.1").
  - `WsrvWarnings.cmake`: function `wsrv_apply_warnings(<target>)` using the exact flag list from Global Constraints, gated by option `WSRV_WARNINGS_AS_ERRORS` (default ON).
  - `WsrvHardening.cmake`: function `wsrv_apply_hardening(<target>)` active only for `Release`/`RelWithDebInfo`; sets PIE via `POSITION_INDEPENDENT_CODE`.
  - `WsrvTests.cmake`: `FetchContent` GoogleTest **v1.15.2 tarball with `URL_HASH SHA256=<computed during this step with curl|sha256sum and pasted in>`**, `include(GoogleTest)`, defines `wsrv_add_test`.
  - Options per BUILD_AND_TOOLING §3; `enable_testing()` when `WSRV_BUILD_TESTS`.
- [ ] **Step 4: Run to verify it passes** — `cmake --build build/t1 && ctest --test-dir build/t1 --output-on-failure` → 2 tests PASS.
- [ ] **Step 5: Negative checks** — run `cmake -S . -B .` (expect refusal) and `CXX=clang++ cmake ...` without libc++ (expect the std::expected message). Record outputs in the commit body.
- [ ] **Step 6: Commit** — `feat(build): CMake skeleton with warnings, toolchain gates, version library`.

### Task 2: Module skeleton targets enforcing the dependency graph

**Files:**
- Create: `src/{crypto,http,protocol,extensions,net,server}/CMakeLists.txt` (each calls `wsrv_add_module` with empty `SOURCES`)
- Modify: `CMakeLists.txt` (add subdirectories in dependency order), `src/base/CMakeLists.txt` (use `wsrv_add_module`)

**Interfaces:**
- Consumes: `wsrv_add_module` (Task 1).
- Produces: targets `wsrv_base`, `wsrv_crypto(base)`, `wsrv_http(base)`, `wsrv_protocol(base crypto http)`, `wsrv_extensions(base protocol)`, `wsrv_net(base)`, `wsrv_server(base protocol net extensions)` and aliases `wsrv::<module>`.

- [ ] **Step 1: Write the failing check** — a CTest test `module_graph` (CMake script `tests/module_graph.cmake` run via `add_test(COMMAND ${CMAKE_COMMAND} -P ...)`) that asserts, using `get_target_property(... LINK_LIBRARIES)`, that `wsrv_protocol` does **not** link `wsrv_net` and `wsrv_net` does **not** link `wsrv_protocol`; and that all seven targets exist.
- [ ] **Step 2: Run** `ctest -R module_graph` → FAIL (targets missing).
- [ ] **Step 3: Implement** the seven `CMakeLists.txt` files with the exact `DEPS` above.
- [ ] **Step 4: Run** `cmake --build build/t1 && ctest --test-dir build/t1` → all PASS.
- [ ] **Step 5: Commit** — `build: module targets encoding allowed dependency graph`.

### Task 3: Presets and sanitizers (with proof they are active)

**Files:**
- Create: `CMakePresets.json`, `cmake/WsrvSanitizers.cmake`, `tests/sanitizer_selftest/CMakeLists.txt`, `tests/sanitizer_selftest/heap_overflow.cpp`
- Modify: `CMakeLists.txt` (include sanitizers; cache var `WSRV_SANITIZER` ∈ none|asan-ubsan|tsan|msan), `cmake/WsrvToolchainChecks.cmake` (when compiler is Clang add `-stdlib=libc++` to compile+link flags)

**Interfaces:**
- Produces: presets `dev` (GCC, Debug, asan-ubsan, tests), `dev-clang` (Clang+libc++, asan-ubsan), `release` (GCC, hardening, LTO), `relwithdebinfo`, `tsan` (Clang+libc++), `msan`/`fuzz`/`bench` (declared), `coverage` (GCC gcov flags), `tidy` (Clang, `CMAKE_CXX_CLANG_TIDY`); matching build/test presets; build dirs `build/<preset>`. `WsrvSanitizers.cmake` exposes `wsrv_apply_sanitizers(<target>)` and sets `-fno-sanitize-recover=all -fno-omit-frame-pointer`.
- Sanitizer self-test: executable `heap_overflow` that deliberately writes one past a `new[]` array, registered with `WILL_FAIL TRUE` under label `sanitizer-selftest`, only when `WSRV_SANITIZER` is `asan-ubsan`.

- [ ] **Step 1: Write the failing test** — `heap_overflow.cpp` main allocating `new char[8]`, writing index 8 through a `volatile` pointer, returning 0; registered `WILL_FAIL TRUE`.
- [ ] **Step 2: Run** `cmake --preset dev && cmake --build --preset dev && ctest --preset dev -L sanitizer-selftest` → expect the preset to be missing (FAIL).
- [ ] **Step 3: Implement** presets + sanitizer module. Run all of `dev`, `dev-clang`, `tsan`, `release` configure+build+ctest in the container.
- [ ] **Step 4: Verify** — `ctest --preset dev` passes (self-test "passes" because ASAN aborts it); `ctest --preset release` has no self-test and the `readelf -d`/`checksec`-style check `readelf -lW build/release/... | grep -q GNU_RELRO` succeeds on the test binary.
- [ ] **Step 5: Commit** — `build: presets for gcc/clang/asan/tsan/release/coverage/tidy with sanitizer self-test`.

### Task 4: Style, lint, banned functions, include graph, header check

**Files:**
- Create: `.clang-format` (LLVM base, `IndentWidth: 4`, `ColumnLimit: 100`, `BinPackArguments: false`, `BinPackParameters: false`, `IncludeBlocks: Regroup`), `.clang-tidy` (profile from CODING_STANDARDS §10, `WarningsAsErrors: '*'`), `.editorconfig`
- Create: `tools/check_banned_functions.sh`, `tools/check_includes.py`, `tools/format.sh`
- Create: `tests/tools/test_check_includes.py`, `tests/tools/test_banned_functions.sh`, `tests/tools/fixtures/` (bad_protocol_includes_net.hpp, bad_strcpy.cpp, good.hpp)
- Modify: `CMakeLists.txt` — targets `format-check` (clang-format `--dry-run -Werror` over `src include tests`), `check-banned`, `check-includes`, `header-check` (for each `src/**/*.hpp`, generate and compile a one-line TU `#include "<header>"` into an OBJECT library with the module's flags)

**Interfaces:**
- Produces: `tools/check_includes.py [--root <dir>]` → exit 0 / 1, prints `file:line: <module> must not include <other>`; encodes the table in ARCHITECTURE §2.1 and additionally bans `<sys/*>`, `<netinet/*>`, `<arpa/*>`, `<unistd.h>`, `<poll.h>`, `<openssl/*>` in `base crypto http protocol`. Project includes use the form `"<module>/<file>.hpp"`. `tools/check_banned_functions.sh [paths...]` → exit 1 listing `file:line:word` for `strcpy|strcat|sprintf|gets|strtok|atoi|atol|rand|srand|system|popen` (whole-word match; scans `src/ include/`).

- [ ] **Step 1: Write the failing tests** — `test_check_includes.py` (unittest, runs the script against fixtures): bad fixture → exit 1 with `must not include`; good fixture → exit 0. `test_banned_functions.sh`: `bad_strcpy.cpp` → exit 1, `good.hpp` → exit 0. Register both with CTest.
- [ ] **Step 2: Run** `ctest -R tools` → FAIL (scripts absent).
- [ ] **Step 3: Implement** the scripts (regex over `#include` lines; module = first path component under `src/`), the config files, and the CMake targets.
- [ ] **Step 4: Run** `ctest -R tools` PASS; `cmake --build --preset dev --target format-check check-banned check-includes header-check` all succeed on the current tree.
- [ ] **Step 5: Negative check** — temporarily create `src/base/bad.hpp` using `std::string` without `#include <string>`; `header-check` must fail; delete the file.
- [ ] **Step 6: Commit** — `build: formatting, lint, banned-function, include-graph and header checks`.

### Task 5: Continuous integration and PR template

**Files:**
- Create: `.github/workflows/ci.yml`, `.github/PULL_REQUEST_TEMPLATE.md`, `.github/ISSUE_TEMPLATE/bug_report.md`, `.github/ISSUE_TEMPLATE/feature_request.md`, `.github/CODEOWNERS` (owner: `@DimathJayasinghe`)

**Interfaces:**
- Consumes: presets and targets from Tasks 3–4; `docker/Dockerfile` stage `dev`.
- Produces: workflow `ci` on `push` to `main` and on `pull_request` with jobs: `lint` (format-check, check-banned, check-includes, header-check), `build-test` matrix `{dev, dev-clang, release}`, `tsan`; every job builds the `wsrv-dev` image via `docker/build-push-action` with GHA layer cache and runs commands inside it as UID 1001/GID 1001 build args. PR template contains the Definition-of-Done checklist from CONTRIBUTING.md and the Security checkbox.

- [ ] **Step 1: Write the workflow and templates.**
- [ ] **Step 2: Validate locally** — run `docker run` equivalents of each job's command inside the dev container (`make shell`), expecting every command to exit 0; run `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml'))"` to prove the YAML parses.
- [ ] **Step 3: Push on a branch** `chore/m0-ci`, open a PR with `gh pr create`, wait for the run with `gh run watch`; expected: all jobs green. Fix and repeat until green.
- [ ] **Step 4: Commit** — `ci: GitHub Actions pipeline, PR and issue templates`.

### Task 6: Docs, lesson, and milestone close-out

**Files:**
- Create: `docs/learning/01-build-system-and-quality-gates.md`
- Modify: `docs/learning/README.md` (lesson 01 → written), `docs/BUILD_AND_TOOLING.md` (replace "planned" statements with the real preset/target names), `docs/PROJECT_PLAN.md` (M0 status line), `README.md` (quick-start commands)

**Interfaces:** none.

- [ ] **Step 1: Write Lesson 01** following the template (Goal → Background → Design → Walkthrough → Tests → Pitfalls → Exercises); must explain why each warning flag exists with one concrete bug it prevents, what each sanitizer catches, why libc++ for Clang, how the link graph enforces architecture, and how to run each gate. Include the real output of the sanitizer self-test failing under ASAN.
- [ ] **Step 2: Verify documentation commands** — copy every command in the lesson and README quick-start into `make shell` and run it; each must succeed as written.
- [ ] **Step 3: Merge** the PR (squash) once CI is green; confirm `main` CI is green.
- [ ] **Step 4: Commit** — `docs: lesson 01 and M0 close-out`.

---

## Self-review

- **Spec coverage:** M0 exit criteria "CI green on empty tree; all lint gates active" → Tasks 4–5; sanitizer presets → Task 3; module skeletons + enforced graph → Task 2; clang-format/tidy → Task 4; test harness → Task 1. SPDX headers: added to every new C++/CMake file as written. CONTRIBUTING checks "ABI check" and "fuzz smoke" are later milestones (no C ABI / parsers yet).
- **Not in M0 (deliberately):** `Result`, buffers, crypto (M1); real fuzz targets (M1+); Autobahn (M6); coverage thresholds (only the preset exists; enforcement starts when there is code to measure).
- **Type consistency:** `wsrv_add_module` / `wsrv_add_test` signatures are identical in Tasks 1–2; target names match ARCHITECTURE §2.1; preset names match BUILD_AND_TOOLING §4 (plus `dev-clang`, an addition recorded in Task 6).
- **Open choice made here:** internal include form is `"<module>/<file>.hpp"` with `src/` as the include root (not mentioned in earlier docs; recorded in CODING_STANDARDS during Task 4).
