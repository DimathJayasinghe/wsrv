# ADR-0006: `Result<T,E>` error handling

- Status: Proposed
- Date: 2026-10-24

## Context
Hostile input makes errors the normal case on parsing and I/O paths; exceptions are costly, invisible in signatures, and must not cross the C ABI.

## Decision
`std::expected<T, Error>` on all runtime paths, `[[nodiscard]]`. Exceptions only for startup/config errors (`ConfigError` from the builder) and are caught at the C ABI and handler boundaries. Invariant violations abort.

## Consequences
Explicit propagation (`TRY`-style macro/helper permitted, documented). User-handler exceptions are contained (close 1011). Destructors `noexcept`.
