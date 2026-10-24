# ADR-0004: epoll behind `IPoller`

- Status: Proposed
- Date: 2026-10-24

## Context
Linux is the v1 target; macOS/Windows/BSD are roadmap items (BINDINGS §6). io_uring is attractive but kernel/feature-dependent.

## Options considered
1. epoll only, no abstraction.
2. io_uring first.
3. epoll (edge-triggered) implementing `IPoller`; others added later.

## Decision
Option 3. Poller interface is readiness-based (not completion-based) to keep kqueue/epoll/wepoll compatible; io_uring may be added behind a different transport/loop implementation if benchmarks justify.

## Consequences
Contract test suite for `IPoller`; OS code confined to `src/net/`. Edge-triggered semantics require read/write-until-`EAGAIN` discipline and fairness budgets (EC-N07/N08).
