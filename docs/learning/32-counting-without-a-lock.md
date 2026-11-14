# Lesson 32 — Counting without a lock (metrics, M10 slice 2)

## Goal
After this lesson you can explain why a metrics label must never come from peer data, how one-writer relaxed atomics give exact totals without a lock or a shared cache line, how a snapshot of many counters can be correct without being atomic, and why a fixed enumeration is also the best test strategy for a metrics page.

## Background: numbers an operator can act on
Without counters an operator cannot tell a connection flood (per-IP refusals, accept errors) from a slow-consumer problem. OPERATIONS promised Prometheus metrics; ADR-0023 decides how.

## 1. A closed set, not a label API
A common metrics API is `counter("requests", {{"path", request.path}}).inc()`. That is a memory-exhaustion attack: every new path makes a new time series that lives for ever. Here `enum class Metric` lists every counter, **one enumerator per (name, label values)**. The close code a peer chose becomes a *class* (`normal`, `protocol_error`, `policy`, `abnormal`, `other`) by a `switch`, so 65 536 possible codes are five series. The number of time series is a compile-time constant, and a test (`Metrics.EveryMetricHasAUniqueNameAndLabelFromTheSafeAlphabet`) checks that every name and label is `[a-z0-9_]`, so nothing ever needs escaping in the output either.

## 2. One bank per loop
A global `atomic` counter incremented by four loops is correct but slow: every increment moves the cache line between CPU cores. Each loop therefore owns a `CounterBank` (an array of `std::atomic<uint64_t>`, aligned to a cache line) and is its only writer; `fetch_add(relaxed)` is a single instruction with no lock. A reader on any thread sums all banks. *Relaxed* means "no ordering between different counters"; each counter on its own is still exact and never goes down, which is all Prometheus needs (`Metrics.AdditionsFromManyThreadsAreExactAndAReaderSeesThemOnlyGrow`, run under TSan).

The banks belong to the **server**, not to the loops: a worker loop is destroyed on its own thread at shutdown, and a scrape that read a freed bank would be a use-after-free. `MetricBanks` hands each loop one bank as it starts and keeps a spare, so a miscounted loop shares a bank instead of failing.

## 3. A snapshot is not a transaction
`Server::metrics()` reads counter after counter while the loops keep counting. The totals can be a few events apart (connections accepted may be read just before a message that arrived a microsecond earlier). Documented, and fine: each series is exact and monotonic, and a rate computed from two scrapes is right. The sum saturates at the maximum instead of wrapping (`ASnapshotSaturatesInsteadOfWrapping`): even a counter obeys the checked-arithmetic rule.

## 4. Where to count
Count where the decision is made, not where the data passes: the refusal reason at the accept path (`Server::on_accept`), the handshake refusal where the connection refuses, the close class where the connection finishes, the accept error in the acceptor. The protocol and http modules were not touched at all; the connection that consumes an event counts it. The old `rejected_` and `ip_limited_` atomics were deleted, and their accessors now read the counters: one source of truth, no two numbers that can disagree.

## Tests
`Metrics.*`, `MetricBanks.*`, `MetricsText.*` (no sockets); `ConnectionTest.*Counted*` (fake transport: stall timer, slow consumer, TLS failure, messages and bytes); `AcceptorTest.AcceptErrorsAreCountedByKind*`; `ServerMetrics.*` (real sockets, one and three loops). Mutation check: remove the counting of bytes, close class and handshake refusals; four of the eight server tests go red.

## Pitfalls
- A label built from request data.
- Reading a per-loop structure from another thread when the loop may already be gone.
- Counting at several layers (the accept path and the shard both counting "accepted"); decide the owner.
- Testing exact equality right after an action: the loop thread counts asynchronously, so tests wait (`eventually`) for the counter instead of sleeping.

## Exercises
1. Why does `ConnectionsAccepted` count in `LoopShard::adopt` and not in `Server::on_accept`? What would a failed hand-off do to the number otherwise?
2. A scrape reads `BytesOut` and then `MessagesOutText`. Can the page show a message without its bytes? Which direction of error is harmless for a rate?
3. Add `wsrv_ping_timeouts_total`. Which files change, and which test must you extend so that nobody can forget its HELP line?
