# ADR-0008: Autobahn, fuzzing, and sanitizers as release gates

- Status: Proposed
- Date: 2026-10-24

## Context
Interop and memory-safety failures are the main trust-breakers for a networking library written in C++.

## Decision
No release or merge to `main` without: Autobahn pass (no FAILED), ASAN/UBSAN/TSAN-clean test runs, fuzz smoke on all targets, coverage thresholds. Details in TESTING_STRATEGY §10.

## Consequences
Longer CI (mitigated with caching, PR subsets, nightly full runs). Contributors must provide tests and, for parser changes, fuzz updates.
