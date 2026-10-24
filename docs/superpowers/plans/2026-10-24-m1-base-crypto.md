# M1 Base & Crypto Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline) to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** The first real building blocks: error/`Result`, checked arithmetic, byte helpers, bounded buffers, injectable clock/logger/random, SHA-1, Base64, and an incremental strict UTF-8 validator, all test-first against published vectors and under every M0 gate.

**Architecture:** Pure, allocation-conscious value types in `base/`, `crypto/`, `protocol/`; the only OS call (`getrandom`) lives in `net/` behind `base::IRandom` (ADR-0012). Errors are `Result<T>` = `std::expected<T, Error>` (ADR-0006). Fuzz targets guard the parsers.

**Tech Stack:** C++23, GoogleTest, libFuzzer (Clang), OpenSSL (test-only differential check, optional).

**Spec:** PROJECT_PLAN.md M1; ARCHITECTURE.md §3.1–3.2 and §3.4 (`Utf8Validator`); CODING_STANDARDS.md §3–4, §7; PROTOCOL_EDGE_CASES.md EC-U01–U09, EC-H07, EC-H09; ADR-0005, ADR-0006, ADR-0012.

## Global Constraints
- All M0 gates stay green: `-Werror` warnings, clang-tidy profile, `check-includes` (base/crypto/protocol: no OS headers), `check-banned`, `header-check`, sanitizers, hardening tests.
- No exceptions on runtime paths; errors via `Result<T>`; `[[nodiscard]]` on `Result`-returning functions; no raw owning pointers.
- Wire-derived integers use checked arithmetic. Byte data is `std::byte` (`ByteView`), text is `std::string_view`.
- Every new header: SPDX line, `#pragma once`, Doxygen with thread-affinity and error behaviour; includes in the form `"module/file.hpp"`.
- Naming per CODING_STANDARDS §2 (types `CamelCase`, functions `snake_case`, members `name_`, constants `kName`).
- Test vectors are the published ones (RFC 3174, RFC 4648, Unicode Table 3-7); each test cites its EC id where one exists.

## Review Focus
- SHA-1 correctness at padding boundaries (55/56/63/64/65 bytes), million-byte input, and any chunking of `update()`.
- Base64 decode strictness: missing/extra/misplaced padding, non-canonical trailing bits, whitespace, `\0`, non-ASCII, length not a multiple of 4.
- UTF-8: overlong, surrogates, > U+10FFFF, truncated tail, a sequence split at every byte boundary, validation continues correctly after `reset()`.
- Buffers: appending past the bound returns `BufferFull` without modifying state; wrap-around ring reads/writes; zero-length operations.
- `checked_add/mul`: operands at the type maximum.

---

### Task 1: Error and Result
**Files:** Create `src/base/error.hpp`, `src/base/error.cpp`, `src/base/checked.hpp`; Test `tests/base/error_test.cpp`, `tests/base/checked_test.cpp`.
**Interfaces (produces):**
`enum class ErrorCategory : std::uint8_t { Protocol, Limit, Io, Tls, Config, Internal };`
`enum class ErrorCode : std::uint8_t { InvalidArgument, OutOfRange, Overflow, BufferFull, InvalidBase64, InvalidUtf8, RandomUnavailable };`
`struct Error { ErrorCode code; std::string_view context; };` (context is a static string, never peer data)
`[[nodiscard]] constexpr ErrorCategory category(ErrorCode) noexcept; [[nodiscard]] std::string_view to_string(ErrorCode) noexcept;`
`template <class T> using Result = std::expected<T, Error>; [[nodiscard]] constexpr std::unexpected<Error> fail(ErrorCode, std::string_view context = {}) noexcept;`
`template <std::unsigned_integral T> [[nodiscard]] constexpr Result<T> checked_add(T, T) noexcept;` and `checked_mul`.
- [ ] RED tests: `to_string` for every code non-empty and distinct; `category(BufferFull)==Limit`, `category(InvalidUtf8)==Protocol`; `fail(Overflow,"x")` yields `!has_value()` with code/context; `checked_add<uint8_t>(250,5)` ok=255, `(250,6)` → `Overflow`; `checked_add<uint64_t>(max,1)` → `Overflow`; `checked_mul<uint32_t>(65536,65536)` → `Overflow`, `(65535,65537)` ok; `checked_mul(x,0)==0`. Run: build fails (headers missing).
- [ ] GREEN: implement; `checked_*` use `__builtin_*_overflow`-free portable form (compare against `max()/b`).
- [ ] Commit `feat(base): Error, Result and checked arithmetic`.

### Task 2: Byte helpers and bounded buffers
**Files:** Create `src/base/bytes.hpp`, `src/base/byte_buffer.hpp/.cpp`, `src/base/ring_buffer.hpp/.cpp`; Tests `tests/base/bytes_test.cpp`, `byte_buffer_test.cpp`, `ring_buffer_test.cpp`.
**Interfaces (produces):**
`using ByteView = std::span<const std::byte>; using MutableByteView = std::span<std::byte>;`
`[[nodiscard]] ByteView as_bytes(std::string_view) noexcept; [[nodiscard]] std::string_view as_chars(ByteView) noexcept;` (the only reinterpret_cast, audited)
`load_be16/32/64(ByteView)` (precondition: size ≥ N, asserted) and `store_be16/32/64(MutableByteView, value)`.
`class ByteBuffer { explicit ByteBuffer(std::size_t max_size); Result<void> append(ByteView); ByteView view() const noexcept; std::size_t size() const noexcept; std::size_t max_size() const noexcept; void consume(std::size_t n) noexcept; void clear() noexcept; };` never allocates `max_size` up front; append that would exceed it returns `BufferFull` and leaves the buffer unchanged.
`class RingBuffer { explicit RingBuffer(std::size_t capacity); std::size_t write(ByteView) noexcept; std::size_t read(MutableByteView) noexcept; ByteView contiguous_front() const noexcept; void consume(std::size_t) noexcept; size()/capacity()/empty()/full() };` `write` accepts as many bytes as fit and returns the count.
- [ ] RED tests: be16/32/64 round-trip and known bytes (`0x0102030405060708` → 01..08); `as_bytes/as_chars` round-trip; ByteBuffer: append within bound, append past bound → `BufferFull` + size unchanged (EC-F06/F22 principle), `consume` then append reuses space, `max_size` 0 refuses any non-empty append, empty append OK; RingBuffer: fill to capacity, partial write count, wrap-around read order preserved over 1000 random ops vs `std::deque` model, `contiguous_front` never exceeds size, zero-length ops.
- [ ] GREEN, commit `feat(base): byte helpers, bounded ByteBuffer and RingBuffer`.

### Task 3: Injected clock, logger, random (+ the one OS adapter)
**Files:** Create `src/base/clock.hpp`, `src/base/logger.hpp`, `src/base/random.hpp`, `src/net/system_random.hpp/.cpp`, `tests/support/fakes.hpp`; Tests `tests/base/clock_test.cpp`, `tests/net/system_random_test.cpp`; Modify `src/net/CMakeLists.txt`, `tests/CMakeLists.txt`.
**Interfaces (produces):**
`using TimePoint = std::chrono::steady_clock::time_point; class IClock { virtual ~IClock()=default; virtual TimePoint now() const = 0; }; class SteadyClock final : public IClock;`
`enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error }; class ILogger { virtual void log(LogLevel, std::string_view message) = 0; }; class NullLogger final : public ILogger;`
`class IRandom { virtual ~IRandom()=default; virtual Result<void> fill(MutableByteView) = 0; }`
`class SystemRandom final : public IRandom` (net): loops on `EINTR`/short reads of `getrandom(2)`; never falls back; returns `RandomUnavailable` on failure.
Test fakes (`tests/support/fakes.hpp`): `FakeClock::advance(duration)`, `RecordingLogger`, `FakeRandom` (deterministic counter bytes).
- [ ] RED tests: `SteadyClock` monotonic (two reads non-decreasing); `FakeClock` advance; `NullLogger` accepts any call; `SystemRandom::fill` of 64 bytes succeeds and two fills differ (probability of failure 2^-512), zero-length fill OK; `FakeRandom` deterministic.
- [ ] GREEN, commit `feat: IClock, ILogger, IRandom interfaces and SystemRandom adapter`.

### Task 4: SHA-1 (RFC 3174)
**Files:** Create `src/crypto/sha1.hpp/.cpp`; Test `tests/crypto/sha1_test.cpp`; Modify `src/crypto/CMakeLists.txt`.
**Interfaces (produces):** `class Sha1 { using Digest = std::array<std::byte, 20>; void update(ByteView) noexcept; [[nodiscard]] Digest finish() noexcept; /* resets to initial state */ [[nodiscard]] static Digest hash(ByteView) noexcept; };` plus `[[nodiscard]] std::string to_hex(const Sha1::Digest&)` in the test support only.
- [ ] RED tests (vectors): `""`=da39a3ee5e6b4b0d3255bfef95601890afd80709; `"abc"`=a9993e364706816aba3e25717850c26c9cd0d89d; `"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"`=84983e441c3bd26ebaae4aa1f95129e5e54670f1; one million `'a'`=34aa973cd4c4daa4f61eeb2bdbad27316534016f; lengths 55,56,63,64,65,119,120 of repeated `'x'` with expected digests generated by Python `hashlib` and embedded; every split of the "abc…" message into two `update()` calls and byte-at-a-time gives the same digest; `finish()` then reuse equals fresh; WebSocket key vector: SHA-1(`dGhlIHNhbXBsZSBub25jZQ==` + GUID) = `b37a4f2cc0624f1690f64606cf385945b2bec4ea` (EC-H09). If OpenSSL is found at configure time, a differential test over 2000 random lengths/contents vs `SHA1()`.
- [ ] GREEN, commit `feat(crypto): streaming SHA-1`.

### Task 5: Base64 (RFC 4648, strict)
**Files:** Create `src/crypto/base64.hpp/.cpp`; Test `tests/crypto/base64_test.cpp`.
**Interfaces (produces):** `[[nodiscard]] std::string base64_encode(ByteView); [[nodiscard]] Result<std::vector<std::byte>> base64_decode(std::string_view);` strict: standard alphabet, padding mandatory, canonical (unused trailing bits zero), no whitespace/newlines/NUL; errors `InvalidBase64`.
- [ ] RED tests: RFC 4648 §10 vectors both ways (`""`,`f`→`Zg==`,`fo`→`Zm8=`,`foo`→`Zm9v`,`foob`→`Zm9vYg==`,`fooba`→`Zm9vYmE=`,`foobar`→`Zm9vYmFy`); decode rejects: `Zg` (no padding), `Zg=` , `Z===`, `=Zg=`, `Zm9v\n`, `Zm 9v`, `Zh==` (non-canonical), `Zm9=`-style bad bits (`Zm9=`→ accepted? no: `Zm9=` decodes "fo"+bits: reject if low 2 bits of last sextet non-zero), `Z\0==`, `Zg==Zg==`, characters `-_` (URL alphabet), non-ASCII byte; `dGhlIHNhbXBsZSBub25jZQ==` decodes to 16 bytes (EC-H07); round-trip over all lengths 0..300 with `FakeRandom` data.
- [ ] GREEN, commit `feat(crypto): strict Base64`.

### Task 6: Incremental strict UTF-8 validator
**Files:** Create `src/protocol/utf8.hpp/.cpp`, `src/protocol/CMakeLists.txt` (sources); Test `tests/protocol/utf8_test.cpp`.
**Interfaces (produces):** `class Utf8Validator { [[nodiscard]] Result<void> feed(ByteView) noexcept; /* fails at the first invalid byte, validator then stays in error state until reset() */ [[nodiscard]] bool at_boundary() const noexcept; /* true when no code point is pending */ void reset() noexcept; }; [[nodiscard]] bool is_valid_utf8(ByteView) noexcept;` — DFA per Unicode Table 3-7 (overlongs, surrogates, > U+10FFFF rejected).
- [ ] RED tests: EC-U01 (2/3/4-byte, U+0000, U+007F, U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF); EC-U02 overlongs (C0 80, C1 BF, E0 80 80, E0 9F BF, F0 80 80 80, F0 8F BF BF); EC-U03 surrogates (ED A0 80, ED BF BF); EC-U04 (F4 90 80 80, F5 80 80 80, F8…, FF); EC-U05 lone continuation 80, truncated E2 82 at end → `feed` ok but `at_boundary()` false; EC-U06 every message split at every byte boundary gives the same verdict as one-shot, and the failure is reported by the chunk containing the first invalid byte (not at FIN); EC-U08 pending state survives `feed` of empty views; after an error `feed` keeps failing until `reset()`; EC-U09 differential against a table-driven reference decoder written in the test: exhaustive all 1- and 2-byte strings, all 3-byte strings with first byte in {E0..EF}, plus 200 000 random strings of 1–8 bytes (seeded).
- [ ] GREEN, commit `feat(protocol): incremental strict UTF-8 validator`.

### Task 7: Fuzz targets
**Files:** Create `tests/fuzz/fuzz_utf8.cpp`, `tests/fuzz/fuzz_base64.cpp`, `tests/fuzz/fuzz_sha1.cpp`, `tests/fuzz/CMakeLists.txt`, `tests/fuzz/corpus/` seeds; Modify `tests/CMakeLists.txt`.
**Interfaces:** `extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t)` per target; built when `WSRV_BUILD_FUZZERS` (preset `fuzz`, Clang). Invariants asserted in the harness: UTF-8 chunked feed == one-shot verdict; `decode(encode(x))==x` and `decode` never crashes on any string; SHA-1 chunked == one-shot.
- [ ] RED: `cmake --preset fuzz` configure (targets missing → build fails). GREEN: targets build; run each 20 s locally with no findings; seed corpus committed.
- [ ] Commit `test: libFuzzer targets for utf8, base64, sha1`.

### Task 8: Lessons, docs, PR
**Files:** Create `docs/learning/02-result-bytes-and-checked-arithmetic.md`, `03-sha1-and-base64.md`, `04-utf8-as-a-state-machine.md`; Modify docs/learning/README.md, PROJECT_PLAN.md (M1 status), BUILD_AND_TOOLING (fuzz preset usage).
- [ ] Lessons written from observed behaviour; all commands in them executed. Open PR, wait CI green, run whole-branch review (fresh reviewer), fix findings, merge.

## Self-review
Spec coverage: M1 exit criteria (RFC 3174/4648 vectors, UTF-8 exhaustive + fuzz) → Tasks 4–7; `Result`, buffers, clock, logger → Tasks 1–3; CSPRNG wrapper → Task 3 via ADR-0012. `BufferPool` is deferred to M5/M6 (no consumer yet) — recorded as a plan ruling. Types consistent across tasks (`ByteView` from Task 2 used in 4–7; `Result`/`Error` from Task 1 used in 2–6).
