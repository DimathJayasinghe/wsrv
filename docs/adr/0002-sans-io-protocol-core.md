# ADR-0002: Sans-I/O protocol core

- Status: Proposed
- Date: 2026-10-24

## Context
Protocol bugs are the highest-risk defect class. Sockets, threads, and clocks make tests slow, flaky, and fuzzing hard. Multiple bindings and future platforms need the same logic.

## Options considered
1. Protocol logic inside connection class using sockets directly — simple, untestable in isolation.
2. Callback-based parser coupled to a loop API.
3. Sans-I/O: pure state machines: `feed(bytes)`, `tick(now)` in; events + output bytes out.

## Decision
Option 3. `protocol/` and `http/` must not include OS or network headers; time via injected `IClock`/passed `now`.

## Consequences
+ Deterministic, fuzzable, portable, trivially reusable by a future client.
− A small adapter (`Connection`) is needed; some buffering copies must be designed away carefully.
Enforced by CMake link rules and include-graph check.
