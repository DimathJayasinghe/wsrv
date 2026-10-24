# Lesson 01 — Build system and quality gates

## Goal
Understand every moving part of the M0 foundation: why the build refuses to compile sloppy code, how bugs that C++ normally hides become loud crashes with file and line, and how the architecture is enforced by machines instead of memory. After this lesson you can run, read, and extend every gate.

## 1. The problem
C++ lets you write code that compiles, runs, and is wrong: a buffer overrun that corrupts memory silently, an integer that wraps, two threads touching one variable. For a network server that reads **hostile bytes**, each of those is a potential remote exploit. We cannot make the language safe, so we make *our build* strict: every mistake class that a tool can detect is detected automatically, on every commit, before a human looks.

## 2. Warnings: the compiler as a reviewer
`cmake/WsrvWarnings.cmake` turns on a long list of warnings and `-Werror` (a warning fails the build). Each flag exists because of a bug class that matters for a protocol parser:

| Flag | Bug it prevents (concrete example) |
|---|---|
| `-Wconversion -Wsign-conversion` | `std::uint16_t declared = 65535; std::uint8_t small = declared;` silently becomes 255. In a frame parser a truncated length means reading the wrong number of bytes. |
| `-Wshadow` | an inner `len` hides the outer `len`; you validate one variable and use the other. |
| `-Wold-style-cast` | `(char*)buffer` hides what the cast does; we want every cast visible and searchable (`static_cast`, `reinterpret_cast`). |
| `-Wimplicit-fallthrough` | `case Opcode::Text:` forgetting `break` and running into the `Binary` case. |
| `-Wnon-virtual-dtor`, `-Woverloaded-virtual` | deleting a derived object through a base pointer (undefined behaviour); an override that silently isn't one. |
| `-Wnull-dereference` | a path where a pointer can be null and is dereferenced anyway. |
| `-Wformat=2` | `printf(user_string)` — an attacker-controlled format string. |
| `-Wcast-align`, `-Wdouble-promotion`, `-Wpedantic` | misaligned reinterpretation, accidental `float`→`double`, non-standard extensions. |
| `-Werror` | warnings nobody reads are noise; warnings that fail the build get fixed. |

Third-party code (GoogleTest) is deliberately *not* given these flags — we only police our own code.

## 3. Sanitizers: instrumented programs that catch bugs at run time
A sanitizer is the compiler inserting extra checks around every memory access or operation. They are slow (2–10×) so they run in tests, not in production.

| Sanitizer | Catches | Preset |
|---|---|---|
| **ASan** (Address) | heap/stack overflow, use-after-free, leaks | `dev`, `dev-clang` |
| **UBSan** (Undefined Behaviour) | signed overflow, bad shifts, null/misaligned access | `dev`, `dev-clang` |
| **TSan** (Thread) | data races — two threads, one variable, no synchronisation | `tsan` |
| **MSan** (Memory) | reading uninitialised memory (needs special libc++; nightly later) | `msan` |

**How do we know they really work?** The `sanitizer-selftest` tests are programs that commit exactly one bug, and the test passes *only if the sanitizer reports it*. For example `tests/sanitizer_selftest/heap_overflow.cpp` writes one byte past an 8-byte array. This is the real output we saw:

```text
==468==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x502000000018 ...
WRITE of size 1 at 0x502000000018 thread T0
    #0 0x561089b362e3 in main .../tests/sanitizer_selftest/heap_overflow.cpp:7
```

and UBSan for `INT_MAX + 1`:

```text
.../signed_overflow.cpp:7:11: runtime error: signed integer overflow: 2147483647 + 1
```

Without those self-tests, a typo in a CMake flag could leave "sanitizers" configured but silently doing nothing — and we would believe our code was checked.

Two details worth knowing:
- Sanitizer flags are applied **globally**, to GoogleTest too. TSan only sees races if *every* piece of code is instrumented.
- `-fno-sanitize-recover=all` makes the first finding abort the program, so a test can never "pass past" a bug.

## 4. Why Clang uses libc++
We use C++23 `std::expected` for error handling (ADR-0006). Clang 18 combined with GCC's library (libstdc++) does not provide it — we found this by compiling a 10-line test, not by reading docs. Clang therefore builds against LLVM's own library, libc++ (`-stdlib=libc++`). `cmake/WsrvToolchainChecks.cmake` proves the toolchain works at *configure time* by compiling `cmake/checks/std_expected.cpp`, and prints a one-sentence fix if it cannot. You can see it fail on purpose:

```bash
CXX=clang++ cmake -S . -B build/neg -DWSRV_USE_LIBCXX=OFF   # fails with a clear message
```

## 5. Architecture enforced by the build
ARCHITECTURE.md says `protocol/` (pure logic) must never depend on `net/` (sockets). A sentence in a document is forgotten; a failing gate is not. Three layers enforce it (the include checker is the primary guard: all modules share the `src/` include path, so the compiler alone would happily accept a stray `#include`):

1. **CMake link graph** (`src/<module>/CMakeLists.txt`) as a second guard. Each module declares only its allowed dependencies; a review of M0 showed that a private link is recorded differently by CMake, so the writer reads every kind of link and a separate test (`module_graph_sees_private_links`) proves it. The test `module_graph` compares the real graph against `tests/module_graph.expected`; adding a forbidden link makes it fail (we tried: adding `wsrv::net` to `protocol` failed the test).
2. **`tools/check_includes.py`**: scans `#include` lines. `protocol` may not include `"net/..."`, and the sans-I/O modules may not include `<sys/socket.h>`, `<unistd.h>`, `<openssl/...>` and friends (ADR-0002).
3. **Header self-containment** (`header-check` target): every header is compiled alone, so a header that forgets `#include <string>` fails immediately instead of working "by accident" when included after another header.

## 6. Formatting, lint, banned functions
- **clang-format** (`.clang-format`): one layout, no style debates. `tools/format.sh fix` rewrites; `format-check` verifies.
- **clang-tidy** (`.clang-tidy`, `tidy` preset): hundreds of bug-pattern and style checks, including *naming rules* (types `CamelCase`, functions `snake_case`, private members `name_`, constants `kName`). We proved it works by planting a global variable and a badly named identifier: it reported three findings.
- **`check-banned`**: fails on calls to `strcpy`, `sprintf`, `gets`, `atoi`, `rand`, `system`... Each has a safe replacement; the list lives in CODING_STANDARDS §7. It matches `name(` so ordinary prose like "operating system" is fine.

The tools themselves are tested (`ctest -L tools`) against small good/bad fixture files — a checker nobody tests is a checker that can silently stop checking. (The test even caught a real bug in our first banned-function script: `grep` omits the filename for a single file, so the output lacked the file name.)

## 7. Presets: named, reproducible configurations
`CMakePresets.json` gives each configuration a name so nobody has to remember flags:

| Preset | For |
|---|---|
| `dev` | everyday work: GCC, Debug, ASan+UBSan |
| `dev-clang` | same with Clang + libc++ |
| `release` | optimised, hardened (PIE, RELRO, stack protector, `_FORTIFY_SOURCE=3`), LTO |
| `tsan` | thread-safety checks |
| `tidy` / `tidy-all` | clang-tidy on production code (what CI runs) / also on tests (slow, local) |
| `coverage` | gcov instrumentation |
| `relwithdebinfo`, `bench`, `fuzz`, `msan` | declared, used from later milestones |

We checked the release hardening on the actual binary: `readelf` shows it is a position-independent executable with a `GNU_RELRO` segment and `BIND_NOW`.

## 8. Running everything
Inside the dev container (`make shell`):

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check check-banned check-includes header-check
ctest --preset dev -L sanitizer-selftest        # prove the sanitizers fire
ctest --preset dev -L tools                     # tests of the tools
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset tidy && cmake --build --preset tidy
./tools/format.sh fix                           # reformat in place
```

## 9. CI: measure before you optimise
CI (`.github/workflows/ci.yml`) runs the same gates on every push. Our first version built the Docker dev image in each of five jobs: **~19 billed minutes**, ~16 of them just building images, for 16–35 seconds of real work per job. After measuring, we switched to the runner's preinstalled compilers in two jobs, skipped docs-only changes, and verify the Docker image in a separate workflow that runs only when `docker/**` changes: **~2.3 minutes**. The lesson: look at the numbers before deciding what to optimise. Actions are pinned to full commit SHAs so a moved tag cannot change what runs.

## 10. Pitfalls we actually hit
1. **Docker Desktop bind mounts can show stale files.** A host file replaced by *rename* (what many editors and tools do) may stay old inside the container, so a build used an empty `version.cpp`. Symptom: "undefined reference" for something you can see on the host. Fix: `make restart`. (CLAUDE.md and BUILD_AND_TOOLING §10.)
2. **Named volumes are created root-owned**, so ccache failed with "Permission denied" until the Dockerfile pre-created the directory with the right owner.
3. **Tests that cannot fail prove nothing.** Every gate here was verified by planting a violation and watching it fail.

## 11. Exercises
1. In a scratch file write `std::uint8_t f(std::uint16_t n) { std::uint8_t b = n; return b; }` and compile it with `g++ -std=c++23 -Wall -Wextra -Wconversion -Werror -c`. Which flag stops you? Fix it with an explicit `static_cast` and a comment explaining why truncation is intended. (Try the constant `std::uint16_t{300}` too: a *different* flag fires, because the compiler can see the value changes from 300 to 44.)
2. Add a use-after-free to a scratch program (`delete p; *p = 1;`) and run it under the `dev` preset. Which part of the ASan report tells you where the memory was freed?
3. Add `wsrv::net` to `protocol`'s dependencies. Which test fails and what does the diff show?
4. Write `#include <unistd.h>` in a new `src/http/x.hpp`. Run all four gate targets: which one fails, and why do the other three pass?
5. Why does `tsan` need GoogleTest instrumented too?

<details><summary>Answers</summary>

1. `-Wconversion`: "conversion from 'uint16_t' to 'uint8_t' may change value [-Werror=conversion]" (an error because of `-Werror`). `static_cast<std::uint8_t>(value)` makes the narrowing deliberate and reviewable. The constant version fires `-Woverflow` ("changes value from '300' to '44'").
2. The "freed by thread T0 here" stack and the "previously allocated by" stack, below the "READ/WRITE of size N" header.
3. `module_graph`: the line `protocol: base crypto http net` differs from the expected `protocol: base crypto http`.
4. Only `check-includes` fails: `src/http/x.hpp:3: http must not include <unistd.h> (sans-I/O layer, ADR-0002)`. The header is perfectly valid C++ — it is self-contained (`header-check` passes), uses no banned function, and is well formatted. Each gate checks one thing; the architecture rule needs its own gate.
5. A race involves two accesses; if one side's code isn't instrumented TSan cannot see its accesses and misses (or misreports) the race.
</details>

## 12. What's next
**M1 — base and crypto.** We write `Result<T>`, byte views with checked arithmetic, SHA-1, Base64 and the UTF-8 validator, test-first against the published RFC vectors — the first real protocol building blocks, running under all of these gates.
