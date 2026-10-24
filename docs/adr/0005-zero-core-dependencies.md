# ADR-0005: Zero third-party dependencies in core

- Status: Proposed
- Date: 2026-10-24

## Context
"From scratch" project, security-sensitive, distributed to many ecosystems (PHP/Python/Node packaging suffers from transitive deps). SHA-1, Base64, UTF-8 validation, HTTP upgrade parsing are small and fully specified with published test vectors.

## Decision
Core modules (`base`, `crypto`, `http`, `protocol`, `net`, `server`) use only the standard library and OS APIs. Optional modules: OpenSSL (TLS), zlib (deflate). Test-only: GoogleTest, Google Benchmark, libFuzzer.

## Consequences
+ Small supply-chain surface, easy packaging.
− We own correctness of SHA-1/Base64 (mitigated by RFC vectors, differential tests against OpenSSL in test builds, fuzzing). Our crypto is **only** for the handshake accept key and is documented as not general-purpose.
