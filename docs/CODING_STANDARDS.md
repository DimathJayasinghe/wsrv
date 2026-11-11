# Coding Standards

Baseline: **C++ Core Guidelines** (https://isocpp.github.io/CppCoreGuidelines). This document lists project-specific rules and overrides. Where silent, follow the Core Guidelines. Rules marked **[CI]** are enforced automatically; **[REVIEW]** are enforced by reviewers.

## 1. Language & compilation

- C++23, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`). **[CI]**
- Warnings (GCC/Clang): `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror`. **[CI]**
- Hardening in release: `-D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS` (libc++: `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST`) `-fstack-protector-strong -fstack-clash-protection -fcf-protection -fPIE -pie -Wl,-z,relro,-z,now`. The library flags turn an out-of-range `std::span`/`vector` index into an abort instead of silent undefined behaviour. **[CI]**
- No `-fno-exceptions` (user handlers may throw; we catch at the boundary) but no exceptions on internal runtime paths.
- RTTI allowed but unused in logic (see DESIGN_PATTERNS §5).

## 2. Naming & layout

| Entity | Style | Example |
|---|---|---|
| Namespace | `lower_case` | `wsrv::protocol` |
| Type (class/struct/enum/concept/alias) | `PascalCase` | `FrameParser`, `CloseCode` |
| Interface (pure abstract) | `I` + `PascalCase` | `ITransport` |
| Function / method / variable | `snake_case` | `feed_bytes`, `max_size` |
| Data member (private) | `snake_case_` | `state_` |
| Struct plain-data member | `snake_case` | `cfg.max_frame_size` |
| Constant / constexpr | `kPascalCase` | `kMaxControlPayload` |
| Enumerator | `PascalCase` (enum class only) | `CloseCode::ProtocolError` |
| Template parameter | `PascalCase` | `template <typename Transport>` |
| Macro (rare) | `WSRV_UPPER_SNAKE` | `WSRV_ASSERT` |
| Files | `snake_case.hpp/.cpp` | `frame_parser.hpp` |

- Headers: `#pragma once`; self-contained (each compiles alone — CI "header check" target). **[CI]**
- Include order: own header, project headers, third-party, std; each group sorted. **[CI via clang-format]**
- Project includes use the form `#include "<module>/<file>.hpp"` with `src/` as the include root (e.g. `"base/version.hpp"`); module-to-module includes must follow the dependency graph, and `base/crypto/http/protocol` must not include OS headers. **[CI: `check-includes`]**
- Complexity is enforced as clang-tidy *cognitive* complexity ≤ 15 (≈ cyclomatic 10), function length ≤ 50 lines, parameters ≤ 4. Suppress only with `// NOLINT(check): reason`. **[CI: `tidy` preset]**
- One public class per header (small helpers may co-locate). Implementation in `.cpp` unless trivial/template/hot-inline.
- Format: `.clang-format` (LLVM base, 4-space indent, 100 cols, `BinPackArguments: false`). **[CI]**
- Max function length ~40 lines, params ≤ 4 (use option structs), nesting ≤ 3, cyclomatic complexity ≤ 10 (clang-tidy `readability-function-cognitive-complexity`). **[CI]**
- No `using namespace` in headers; none in .cpp except in narrow scopes for literals. **[CI]**

## 3. Types, ownership, and memory

- **RAII for every resource** (fd, SSL*, timers, threads). No naked `new`/`delete`/`malloc`/`free`. **[CI: cppcoreguidelines-owning-memory]**
- Ownership: `std::unique_ptr` default; `std::shared_ptr` only for genuinely shared immutable data (broadcast frames, config snapshots) — justify in a comment.
- Non-owning view: `T&`, `T*` (nullable, non-owning), `std::span`, `std::string_view`. Document lifetime for any stored view.
- Never store a `string_view`/`span` beyond the call unless the owner outlives it by construction (and say so).
- Use `std::byte` + `std::span<const std::byte>` for binary data; `char` only for text. No `reinterpret_cast` outside one audited helper (`as_bytes`, `as_chars`) in `base/`.
- Fixed-width integers (`uint8_t`, `uint64_t`) for wire data; `size_t` for sizes; never mix signed/unsigned in comparisons. Use `std::in_range`, `std::cmp_*`, or checked helpers.
- **Integer overflow is a bug.** Arithmetic on attacker-controlled lengths goes through `checked_add/checked_mul` (return `Result`) — never raw `+` on wire-derived values. **[REVIEW + fuzz]**
- Byte order: explicit `load_be16/64`, `store_be*` helpers; never cast a buffer pointer to `uint16_t*` (alignment/aliasing UB).
- `const` by default; `constexpr` where possible; `[[nodiscard]]` on all `Result`-returning functions and on factories. **[CI]**
- `enum class` always; `explicit` on single-arg constructors and conversion operators.
- Rule of zero; if you write one special member, define/delete all five. Interfaces: virtual protected/public dtor per Core Guidelines C.35; non-copyable.
- Prefer composition; `final` on leaf classes; `override` always. **[CI]**
- Avoid `std::function` on hot paths (allocation, indirection) — use `std::move_only_function`, templates, or interface references. Allocation-free small-buffer callable for timers.

## 4. Error handling

| Situation | Mechanism |
|---|---|
| Expected runtime failure (bad input, I/O error, limit hit) | `Result<T>` (`std::expected<T, Error>`) |
| Invalid configuration at startup | exception `ConfigError` (only in builder/`main`) |
| Programmer error / violated invariant | `WSRV_ASSERT` (always active, also in release: prints the failed condition and calls `std::abort()`) |
| User handler throws | caught at the handler boundary → log → close 1011; never propagates |
| Allocation failure | `Error{ResourceExhausted}` where it is peer-driven and recoverable (e.g. `ByteBuffer::append`); in small, bounded helpers (e.g. `base64_encode/decode` on handshake-sized input) `std::bad_alloc` may propagate and terminate the process |

Rules:
- Every error path is tested. Error contexts are static strings (no allocation, no peer data inside).
- Do not use `errno` after another call; capture immediately into `Error`.
- Never ignore syscall results; wrap each syscall in a helper returning `Result` and retrying `EINTR`.
- Destructors are `noexcept` and never throw/log-fail.
- No error codes as `int`/`bool`; no `catch (...)` that swallows (allowed only at the handler boundary with logging).

## 5. Concurrency

- Annotate each class: `// Thread-affinity: loop-thread-only | any-thread | immutable`.
- Loop-thread-only objects assert affinity in debug (`WSRV_ASSERT_ON_LOOP()`).
- Cross-thread communication only via `EventLoop::post` or atomics/immutable snapshots. No `std::mutex` in per-connection or protocol code. Mutexes allowed only in cold paths (e.g., config reload) and must be documented with what they guard.
- Atomics: default `memory_order_seq_cst`; relaxed only for pure counters, with a comment. No lock-free structures written from scratch without an ADR + TSAN stress tests (MPSC queue is the one exception, covered by ADR 0003).
- Threads are owned by `LoopThread` (RAII, `std::jthread`); no detached threads.
- Signals: handled via `signalfd`/self-pipe in `main`; no logic in signal handlers; `SIGPIPE` ignored and `MSG_NOSIGNAL` used.

## 6. Hot-path rules (protocol, net)

- No heap allocation per message in steady state (NFR-P4): reuse buffers from the pool.
- No `std::string` copies of payloads; pass views.
- No virtual call per byte; virtual per frame/read is OK.
- No logging above `debug` per frame; sampling for noisy paths.
- Optimise only with a benchmark in `tests/bench` demonstrating the win, and keep the simple version available under test as a reference oracle (differential test).

## 7. Input handling

- All peer input is hostile until validated. Validate at the boundary (parser), then use typed values.
- Parse into immutable, validated structs (`HandshakeRequest`, `FrameHeader`); downstream code never re-validates raw bytes.
- Every loop over input has an upper bound derived from a configured limit.
- Prefer table-driven / DFA parsers over ad-hoc `find`/`substr` chains. No `sscanf`, `strcpy`, `sprintf`, `strtok`, `atoi`. **[CI: banned-functions check]**
- Case-insensitive header compare via ASCII-only helper (never locale-dependent `tolower`).

## 8. Comments & documentation

- Code says *what*; comments say *why* (invariants, RFC citations, trade-offs). Cite RFC sections: `// RFC 6455 §5.5.1: close payload MUST be 0 or >= 2 bytes`.
- Public headers: Doxygen (`@brief`, `@param`, `@return`, thread-affinity, ownership, error behaviour). **[CI: -Wdocumentation on clang]**
- No commented-out code. TODOs must reference an issue: `// TODO(#123): ...`. **[CI]**
- Update docs/ADRs in the same PR as the behaviour change.

## 9. Testing conventions (see TESTING_STRATEGY.md)

- Test names: `Suite.Behaviour_Condition_Expected`, e.g. `FrameParser.Rejects_UnmaskedClientFrame`.
- Test the contract, not the implementation. No sleeping; use fake clock/loop.
- Every bug fix lands with a regression test (and fuzz corpus entry if input-driven).

## 10. Static analysis & sanitizers

- `clang-tidy` profile: `bugprone-*, cert-*, clang-analyzer-*, concurrency-*, cppcoreguidelines-* (selected), misc-*, modernize-*, performance-*, readability-*`; exact list in `.clang-tidy`, warnings = errors. **[CI]**
- `cppcheck` secondary pass. **[CI]**
- Sanitizers: ASAN+UBSAN on every test run; TSAN on threaded tests; MSAN nightly where toolchain allows. **[CI]**
- `-fanalyzer` (GCC) nightly. **[CI]**

## 11. Dependencies

- Core: standard library only. Optional: OpenSSL (TLS), zlib (deflate). Test-only: GoogleTest, Google Benchmark, libFuzzer/AFL++, Autobahn.
- New dependency = ADR + license check + pinned version + justification versus writing 100 lines ourselves. See [BUILD_AND_TOOLING.md](BUILD_AND_TOOLING.md#dependency-policy).

## 12. Examples

```cpp
// Good: validated, bounded, checked arithmetic, explicit error, RFC citation
[[nodiscard]] Result<FrameHeader> FrameParser::parse_length(ByteView in) noexcept {
    // RFC 6455 §5.2: payload len 126 => next 2 bytes, 127 => next 8 bytes (MSB must be 0)
    ...
    if (len > cfg_.max_frame_payload) {
        return std::unexpected(Error{ErrorCode::FrameTooLarge, "payload exceeds limit"});
    }
}

// Bad: unchecked, raw pointer math, int sizes, swallowed error
int n = *(uint16_t*)(buf + 2);          // alignment UB, endianness bug, signed
char* p = new char[n]; read(fd, p, n);  // unbounded, owning raw ptr, ignores result
```
