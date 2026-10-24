# ADR-0001: C++23 baseline

- Status: Proposed
- Date: 2026-10-24

## Context
We need `std::expected`, `std::span`, concepts, `std::move_only_function`, `std::byteswap`, `std::jthread`. Dev toolchain is GCC 15.2.

## Options considered
1. C++17 + third-party (`tl::expected`, `span-lite`) — wider compiler reach, extra deps (conflicts with ADR-0005).
2. C++20 + home-grown `Result` — moderate reach, we maintain a `expected` clone.
3. C++23 — everything in std; needs GCC ≥ 13 / Clang ≥ 17.

## Decision
C++23 for the core. The public **C ABI** (ADR-0009) keeps consumers independent of the C++ standard level.

## Consequences
Older distro compilers unsupported for source builds (prebuilt binaries mitigate). Revisit if adoption is blocked by compiler floor.

Finding (2026-10-24): Clang 18 + libstdc++ has no `std::expected`; Clang builds must use libc++ (see BUILD_AND_TOOLING §1.1). If a supported compiler/stdlib pair lacks `std::expected`, `base/result.hpp` provides a minimal in-tree fallback behind the same `Result<T,E>` alias rather than adding a dependency (ADR-0005).
