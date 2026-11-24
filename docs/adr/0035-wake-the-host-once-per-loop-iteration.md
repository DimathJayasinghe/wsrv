# ADR-0035: The host's wake-up is paid once per loop iteration, not once per message

- Status: **Accepted** (2026-11-24; found by the one-CPU comparison in the 1.0 benchmark)
- Date: 2026-11-24
- Deciders: project owner, maintainer assistant
- Amends: ADR-0016 (the host event queue; recorded as its A3). Extends ADR-0013 (the handler factory gains a hook with a default) and ADR-0014 (a loop runs it once per iteration); their decisions stand.

## Context
[docs/BENCHMARKS.md](../BENCHMARKS.md) showed that with everything pinned to **one CPU**, wsrv did 35 to 46 percent fewer echo messages per second than single-process PHP servers, while with four CPUs it was ahead. The release notes said the cause was "the daemon-to-worker hop" and that it had not been profiled. It has now been, on the one-CPU echo run (512 connections, one loop thread):

- **CPU per process:** the daemon used 12.3 CPU-seconds and the PHP worker 5.6. The C++ side, not PHP, was 69 percent of the work.
- **CPU per daemon thread:** the loop thread 8.8 s; the IPC thread 3.8 s with **846 000 voluntary context switches in a run of about 790 000 messages**: the IPC thread was woken about once per message.
- **Syscalls** (strace, so each is slower than real, but the counts hold): about 5.7 per echoed message, against about 3 for a single-process server. Among them an eventfd write and read, futex wake-ups, and a second `read` that only finds the socket empty.

The cause is in `EventQueue`. `push()` wrote the queue's eventfd whenever the queue went from empty to non-empty, and the `try_pop()` that emptied it read the eventfd back. The producer is a loop thread and the consumer is the IPC thread. When both share one CPU the scheduler runs the consumer right after the push, so the queue is empty again at the next push and **nothing is ever batched**: every message costs an eventfd write, an eventfd read, a futex wake, an epoll wake-up and two context switches. With several CPUs the consumer runs elsewhere while events pile up, so batching happens by itself. That is why wsrv scaled with cores and lost per core.

## Decision
**The loop pays the host's wake-up once at the end of each iteration, for every message it queued during it.**

1. `EventQueue::push(event, Wake)`: `Wake::Now` (the default, unchanged) writes the eventfd inside `push()`. `Wake::AtFlush` only records that a wake-up is owed. `EventQueue::flush_wake()` pays it: one eventfd write for the whole burst. A host that empties the queue before the flush cancels the owed wake-up (nothing was written, so nothing needs clearing). Nothing owed means no lock is taken.
2. `EventLoop::set_iteration_end(hook)`: the loop runs the hook once at the end of every iteration, on its own thread, after I/O events, timers, tasks and deferred tasks. The loop contains and counts a throwing hook like a task failure, but the factory's hook is `noexcept` (it takes a lock and writes a descriptor), so in practice it does not throw. It is per loop and set before the loop runs.
3. `IHandlerFactory::on_loop_iteration_end()` (default: nothing): `LoopShard` installs a hook that calls it, so every loop reaches the factory without a global and without the handlers knowing the loop.
4. `HostBridge` implements it with `events_->flush_wake()`, and its handlers push **Text and Binary** with `Wake::AtFlush`. Open, Close, Error and Handshake events are rare and keep `Wake::Now`.

**Why this cannot lose a wake-up.** A wake-up is owed only when a push finds the queue empty. Every `AtFlush` push comes from a handler callback on a loop thread, and that loop runs its hook at the end of the same iteration, so the pusher itself always pays what it owes (the optional lock-free check only reads the pusher's own earlier store). Events pushed from anywhere else, or at shutdown outside an iteration, use `Wake::Now`, which has no deferred part of its own. (A `Now` push that finds the queue **non-empty** writes nothing, as before: it relies on the wake-up already written, or owed to the loop that queued the first event and that pays it at the end of its iteration. So a rare event can also wait for another loop's iteration to end.) A flush that fails (the write returns an error) leaves the wake-up owed and is retried by the next flush, from any loop's next iteration; an idle server has none until the next I/O, and the events stay queued for a host that polls.

**What it costs.** The host learns of the first message of a burst at the end of the loop's iteration instead of at once. An iteration is bounded by the fairness budgets (256 events, 256 tasks and 256 timers) plus the work those run, so the delay is not a time bound, but in practice it is the time to handle one batch of ready connections (microseconds to a few milliseconds), and an idle loop's iteration is immediate, so at low load nothing changes; under load the batching is the point. The host can see the queue fill during the iteration, so the queue's high-water mark (backpressure, ADR-0016 section 4) still works on the queue's size, not on the wake-up. One more thing is **not measured**: with several loop threads the first loop to flush also pays the wake-up owed by the others (a harmless early wake), and the multi-core effect is covered by the 4-CPU benchmark rerun, not by this ADR's one-CPU table.

## Rejected
- **Delay the consumer with a timer.** Adds a fixed latency to every message at low load to fix a problem that only exists at high load.
- **`sched_yield()` or priority tricks in the IPC thread.** Depends on the scheduler; it would only help by accident.
- **Merge the IPC thread into a loop thread.** A restructuring of the daemon for what a flag and a hook solve.
- **A process-wide hook (a global).** Banned by the project rules, and it would make two servers in one process share a hook.
- **Defer every event type.** A Close queued while the server shuts down, outside any iteration, could be stranded without its wake-up.

## Consequences
- + One-CPU echo throughput rises sharply (see Result), the IPC thread no longer wakes per message, and the same batching helps the PHP worker, which now reads many events per `read`.
- + Multi-core runs also do fewer wake-ups and context switches.
- + No new thread, dependency or global; the queue's behaviour for every non-message event is unchanged.
- - A message's wake-up can wait for the rest of its loop iteration (bounded as above).
- - `IHandlerFactory` gains a virtual with a default; third-party factories are unaffected.

## Verification
- `EventQueueTest`: a deferred push does not make the descriptor readable; the flush does, once for a burst; a host that drains first cancels the owed wake-up; an immediate push while a wake-up is owed does not lose it; a burst producer with a concurrent consumer (run under TSan) is never left asleep.
- `EventLoopTest`: the hook runs once per iteration, after tasks and deferred work, every iteration, and an exception is contained.
- `HostBridgeWakeTest`: an Open wakes at once; Text and Binary wait for `on_loop_iteration_end()` and then wake once; a host that polls early loses nothing.
- `HostBridgeTest.ARealLoopWakesAHostThatOnlyWaitsOnTheDescriptor`: with a real server and a host that waits on the descriptor alone, the loop's own hook delivers the wake-up. Mutation checked: with deferral off the bridge test fails, with the hook not installed this test fails.

## Result
(measured 2026-11-24, one CPU, one loop thread, 512 connections, release build, alternating runs)

| One CPU, 512 connections, 64-byte echo | before | after |
|---|---|---|
| messages per second, three alternating runs | 53 628, 52 025, 53 908 | **82 763, 82 784, 84 081** (median +54%) |
| median latency (p50) | 9.6 to 9.7 ms | 5.9 to 6.0 ms |
| IPC thread voluntary context switches per run | 914 000 to 957 000 | 12 000 |
| IPC thread CPU | 3.75 s | 1.73 s, while handling about 1.5 times the messages |
| `futex` / `write` calls in a 5 s sample (strace) | 22 550 / 12 407 | 582 / 515 |

The prototype of this change, with a process-wide hook that was thrown away, measured +43% to +72% in three runs. Single-process Workerman and Swoole reach about 108 000 messages per second in the same setting (docs/BENCHMARKS.md), so the gap at one CPU narrows from about 50 percent to about 23 percent. What is left per message is the loop thread's own work (two `read` calls per ready socket, one `sendto`, the frame and event allocations).

