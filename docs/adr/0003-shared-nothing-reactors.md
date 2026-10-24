# ADR-0003: Shared-nothing reactor-per-thread concurrency

- Status: Proposed
- Date: 2026-10-24

## Context
Need many concurrent connections with low latency and no lock contention; connection state is mutated by I/O events, timers, and application sends.

## Options considered
1. Thread-per-connection — simple, does not scale.
2. Single reactor + worker pool — shared connection state needs locks; cross-thread ordering issues.
3. N reactors, each connection pinned to one; cross-thread via MPSC task queue + eventfd.

## Decision
Option 3. Handlers run on the loop thread; `SessionHandle` posts work to the owning loop. The only hand-written lock-free structure is the bounded MPSC queue, with TSAN stress tests.

## Consequences
+ No locks on connection state; predictable ordering per connection.
− A blocking handler stalls its loop (mitigated: loop-lag metric, docs, binding layer moves user code off I/O threads — see ADR-0009).
− Broadcast needs cross-loop fan-out via `post`.
