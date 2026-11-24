# Lesson 49 — A wake-up per message: finding a hidden ping-pong between threads

## Goal
After this lesson you can find where a server spends its CPU without a profiler (per process, per thread, context switches, syscall counts), explain why two threads on one core can hand every message back and forth, and fix it by batching the wake-up (ADR-0035).

## The problem
The benchmark ([docs/BENCHMARKS.md](../BENCHMARKS.md)) had an uncomfortable row: with everything pinned to **one CPU**, wsrv did 35 to 46 percent fewer echo messages per second than a single-process PHP server, although with four CPUs it was ahead. The first guess was "the daemon-to-worker hop is expensive, so PHP must be the slow part". A guess is not a measurement.

## 1. Measure where the CPU goes (no profiler needed)
`perf` does not work inside Docker Desktop's VM (its kernel has no perf events), so use what Linux always has:

- **Per process:** `/proc/<pid>/stat`, fields 14 and 15 (user and system ticks, 100 per second). Read it before and after a run. Result: the C++ daemon used 12.3 CPU-seconds, the PHP worker 5.6. The guess was wrong: **69 percent was the C++ side.**
- **Per thread:** `/proc/<pid>/task/<tid>/stat` and `comm`. The daemon has a loop thread (8.8 s) and an IPC thread (3.8 s).
- **Context switches:** `/proc/<pid>/task/<tid>/status`, `voluntary_ctxt_switches`. A thread that sleeps because it has nothing to do increments it. The IPC thread had **846 000 in a run of 790 000 messages: it went to sleep and was woken about once per message.** That is the smoking gun for a ping-pong.
- **Syscalls:** `strace -c -f -p 1` from a throwaway container that shares the PID namespace (`docker run --pid=container:<id> --cap-add SYS_PTRACE`, as root). strace makes every call slower, so use it for **counts**, not for times: 5.7 syscalls per message where a single-process server needs about 3.

## 2. The cause
The queue that carries events from the loop thread to the IPC thread had a "wake-up" file descriptor (an `eventfd`). `push()` wrote to it whenever the queue went from empty to non-empty, and the `pop()` that emptied the queue read it back.

```
loop thread:  push(msg)  -> queue was empty -> write(eventfd)      ... wakes the IPC thread
IPC thread:   wakes, pop() -> queue empty again -> read(eventfd)   ... sleeps
loop thread:  push(next msg) -> queue was empty again -> write ...
```

With **two CPUs** the IPC thread runs on the other core while the loop thread keeps pushing, so events pile up and one wake-up serves many messages. With **one CPU** the scheduler lets the woken thread run right away, the queue is empty at the next push, and every single message pays an eventfd write, an eventfd read, a futex wake, an epoll wake-up and two context switches. Nothing batches. That is why wsrv *scaled with cores* and *lost per core*: the same code was fine when the hardware hid the problem.

## 3. The fix: pay the wake-up once per burst
A server loop already has a natural batch: one iteration handles every socket that was ready. So:

1. `push(event, Wake::AtFlush)` records "a wake-up is owed" and does **not** write.
2. The loop calls a hook at the **end of the iteration**; the hook calls `flush_wake()`, which writes the eventfd **once** for the whole burst.
3. If the host drains the queue first, the owed wake-up is cancelled (nothing was written, so nothing needs clearing).

Only the hot path (Text and Binary) defers. Open, Close and the other rare events still wake at once, so nothing can be stranded at shutdown when no iteration follows.

How the hook reaches every loop without a global: the server already hands every loop one `IHandlerFactory`, so it got one new virtual, `on_loop_iteration_end()`, and each `LoopShard` installs a per-loop hook that calls it.

## 4. Check it twice
- Alternating A/B runs (baseline, fix, baseline, fix, ...) on the same machine, so drift cannot favour either: **53 600 to 82 800 messages per second**, median latency 9.6 to 5.9 ms, the IPC thread's voluntary context switches from about 900 000 to 12 000.
- **Mutation checks.** A test that cannot fail proves nothing, so the tests were run against two deliberately broken builds: deferral switched off (a bridge test failed) and the hook never installed (the first version of the end-to-end test still passed, because the helper falls back to polling after a timeout; it was replaced by one that waits on the descriptor alone, and then failed as it should).

## Pitfalls
- A deferred wake-up is only safe if something is **guaranteed** to pay it. Here the pusher's own loop pays it at the end of the same iteration; a push from anywhere else uses the immediate path.
- A failed flush must leave the wake-up owed, not forget it.
- Do not trust a throwaway prototype: it used a process-wide hook. It proved the idea; the real change had to be designed without a global.
- A profile on one machine says where the time went *there*. Say what you measured on (here: one CPU in a Docker VM) and what you did not.

## Exercises
1. Why is it safe that `flush_wake()` returns early, without the lock, when the "owed" hint is false? Whose store does the pusher always see?
2. The queue empties between a deferred push and the flush. Walk through why no stale wake-up is written, and what the host sees.
3. What would break if `Close` events used `Wake::AtFlush` and the server was shutting down?
4. A single-process server never has this problem. What does it pay instead? (Think about where its application code runs.)
5. Find the second `read` per ready socket in the strace counts. Under which conditions may a loop skip it, and what would you test first?
