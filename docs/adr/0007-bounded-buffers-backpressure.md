# ADR-0007: Bounded buffers and backpressure

- Status: Proposed
- Date: 2026-10-24

## Context
Unbounded queues turn slow peers and slow hosts into memory-exhaustion DoS. Applies to sockets, outbound queues, loop task queues, and the binding event queue.

## Decision
Every buffer/queue has a configured limit with hard compile-time ceiling and a defined overflow action (pause reading, drop with metric, or close with code). Declared frame lengths are never trusted for allocation. Read interest is disabled when downstream is saturated.

## Consequences
Extra state machines for pause/resume (tested via EC-N09/N10); some throughput tuning needed; slow consumers get disconnected by default rather than degrading everyone.
