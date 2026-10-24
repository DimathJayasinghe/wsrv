# ADR-0012: Randomness behind `IRandom`; `crypto` stays free of OS calls

- Status: Proposed
- Date: 2026-10-24

## Context
ADR-0002 keeps the protocol core free of OS dependencies, and `tools/check_includes.py` enforces that for `base`, `crypto`, `http` and `protocol`. The architecture draft placed `Random::fill` (a `getrandom(2)` wrapper) in `crypto`, which needs `<sys/random.h>` and therefore contradicts the rule (found in the M0 review). A server needs little randomness: the accept key is deterministic, server frames are unmasked, and connection IDs are counters. Randomness is needed for client masking keys (future client), PROXY/session identifiers and token helpers.

## Options considered
1. Allow an exception for `<sys/random.h>` in `crypto` — weakens the rule and tests become non-deterministic.
2. Interface `IRandom` in `base` (pure), OS adapter `SystemRandom` in `net` (the only module allowed OS calls), injected from the composition root.

## Decision
Option 2. `base/random.hpp` declares `IRandom`; `net/system_random.{hpp,cpp}` implements it with `getrandom(2)` (loop on `EINTR`/short reads; fail closed — never fall back to `rand()` or `/dev/urandom` silently). Tests inject a deterministic `FakeRandom`.

## Consequences
`crypto` is pure and trivially testable; the OS dependency is visible at the composition root; randomness is not available in code that has no injected `IRandom` (intended).
