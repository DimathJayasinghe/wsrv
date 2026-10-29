# Lesson 10 — Sockets, epoll, and the event loop

## Goal
Everything that touches the operating system, kept in one module (`src/net/`) and hidden behind small interfaces: owning file descriptors, calling the kernel, waiting for readiness, running timers and cross-thread tasks, and accepting connections without ever being knocked over by running out of descriptors. After this lesson you can read `src/net/` top to bottom and say why each class exists.

## 1. The problem in one picture
A WebSocket server holds thousands of idle TCP connections. One thread per connection wastes memory and scheduler time; polling each socket in a loop wastes CPU. The OS offers a better question: *"tell me which of these descriptors are ready."* On Linux that is **epoll**. A thread that asks it, then handles whatever is ready, then asks again is a **reactor** (ADR-0003). Our `EventLoop` is that thread's loop.

```text
 loop iteration:  epoll_wait  ->  fd events  ->  due timers  ->  posted tasks  ->  (again)
                  (sleeps)        (<=256)         (<=256)          (<=256)
```
Each source has a **budget** per iteration, so none can starve the others (EC-N07, N08, N15). What exceeds the budget runs next iteration.

## 2. Layers, bottom to top
| Class | One-line job | Why it exists |
|---|---|---|
| `Fd` | RAII owner of a descriptor | A leaked fd is a leaked connection slot. Move-only, closes exactly once. |
| `map_errno` / `ErrorCode::WouldBlock, ConnectionClosed, FdLimit, AddressInUse, Io` | `errno` to `Error` | `EAGAIN` is *normal*, `EPIPE` is *a closed peer*, `EMFILE` is *overload*: callers react differently, so they get different codes. |
| `ISyscalls` / `LinuxSyscalls` / `FakeSyscalls` | Every kernel call behind one interface | Tests inject `EINTR`, `EMFILE`, `EPIPE` on demand. Without this seam the error paths would never run in tests. |
| `retrying(...)` | Retry `EINTR` (up to 64 times) | A signal interrupting a call is not a failure (EC-N01). |
| `TimerWheel` | O(1) arm/cancel timers, no OS | Every connection has several timers (Lesson 09). A sorted list would be O(n). |
| `IPoller` / `EpollPoller` | Register fds, wait for events | The one place epoll lives; macOS/Windows get their own poller later (ADR-0004). |
| `EventLoop` | Poller + wheel + task queue + wake-up | The reactor. |
| `ITransport` / `PlainTransport` | `read`/`write` bytes on a socket | TLS (M8) will be a decorator over the same interface. |
| `TcpListener`, `Acceptor` | Listening socket, accept loop | Where overload attacks arrive first. |

`tools/check_includes.py` enforces the boundary: only `net/` may include OS headers, and `net/` never includes `protocol/`. The protocol core therefore stays testable without a socket.

## 3. Edge-triggered epoll, and the rule that follows
epoll has two modes. *Level-triggered*: "this fd is readable" is reported every time you ask while data remains. *Edge-triggered* (ours): it is reported **once, when new data arrives**. Fewer wake-ups, but one strict rule: **on `Read`, read until `WouldBlock`; on `Write`, write until `WouldBlock` or the queue is empty.** If you stop early, no new edge comes and the connection hangs silently.

Fairness collides with that rule: a flooding peer could make "read until empty" last forever. The cure (used by the `Acceptor` already, by `Connection` in M6): stop after a budget and call `rewatch`, which re-arms the edge so the kernel reports the still-pending data next iteration.

## 4. Watches, generations and the fd-reuse hazard (EC-N12)
`watch(fd, interest, handler)` returns a `WatchHandle{index, generation}`, and the poller's *token* is built from those two numbers, not from the fd number. Why? Picture one iteration:

1. `epoll_wait` returns events for fd 7 (connection A) and fd 9.
2. Handling fd 9 closes fd 7 and the acceptor immediately accepts a new client, which the kernel numbers **7** again (connection B).
3. The harvested event for "fd 7" is still in our batch. Delivered by fd number, A's event would hit B.

With a generation in the token, `unwatch` bumps the generation of A's slot; the stale event no longer matches and is dropped. Test: `EventLoop.*` fd-reuse and unwatch-in-callback cases.

## 5. Tasks, wake-ups, and a bug we found by repeating a test
`post(task)` may be called from any thread (and `stop()` too); everything else is loop-thread-only. A bounded queue (`max_queued_tasks`, `BufferFull` beyond it, EC-N14) holds the tasks. If the loop is asleep inside `epoll_wait`, a producer must wake it by writing to an **eventfd** (a kernel counter that is readable when non-zero) that the loop watches.

Writing the eventfd on every post is wasteful, so many posts should share one write. Our first version used an atomic flag `wake_pending_`: producers exchange it to `true` and write only if it was `false`; the loop clears it when it wakes. It looked fine and passed. Repeating the stress test (`--gtest_repeat`) hung once in a while, until the 60-second idle cap woke the loop. The interleaving:

```text
 loop:      drain eventfd -> (clears flag after / before?) ...
 producer:  push task; flag already true -> skip write
```
If the flag is set *between* the loop clearing it and the loop reading the queue, you reach "flag true, eventfd already drained, queue still holds a task nobody will look at before the next wake-up", and every later post sees flag `true` and skips the write. A lost wake-up: the loop sleeps with work queued.

The fix is to make the flag and the queue **one decision**: `wake_requested_` is a plain `bool` read and written only while holding `tasks_mutex_`, the same lock that guards the queue. The loop clears it in the same critical section in which it pops tasks (`run_tasks`). Now there are only two cases: a producer sees the flag set, so the loop has not popped yet and *will* see this task; or it sees it clear, so it writes the eventfd after pushing and the next `epoll_wait` returns. The write itself happens *outside* the lock (never do a syscall under a lock you don't have to).

The general lesson: two pieces of state that must change together (flag + queue) need one lock, even if each alone "could be an atomic".

Related rules, each with a test: a task posted before `run()` is not lost; a task posted from a task runs next iteration (no unbounded recursion); the loop catches exceptions from tasks (`task_failures()`), because a callback bug must not kill every connection.

## 6. Timers: the wheel
A *hashed timing wheel* is an array of slots, each slot a bucket of timers; a hand advances one slot per tick (10 ms). Arming a timer = compute its slot from the deadline and append: O(1). Cancel = remove by id: O(1). Timers farther than one revolution carry a round count. Only the **monotonic** clock is used (EC-N16): wall time can jump backwards (NTP), and a connection would never time out.

Subtleties we tested: a timer must not fire *before* its deadline (it may fire up to one tick after); a deadline in the past fires on the next advance; 1,000 timers due in one tick are released `budget` at a time and stay in a "ready set" so the cost of a call is proportional to the budget, not to the backlog (an early version re-scanned all due timers per call; the review caught it, commit `d32e2d9`); and a 20,000-operation random model test compares the wheel with a `std::multimap` oracle.

## 7. The Acceptor and the descriptor-exhaustion attack (EC-N05)
Every process has a descriptor limit (`ulimit -n`). An attacker (or a busy day) opens connections until `accept` fails with `EMFILE`. The nasty part: the pending connection is still in the kernel's accept queue, so epoll keeps reporting the listener as readable and a naive loop spins at 100% CPU failing the same `accept`.

The **reserve-fd trick**: at startup open `/dev/null` and keep it. On `EMFILE`:
1. close the reserve (one descriptor is now free);
2. `accept` the waiting connection(s) and close them at once. The peer sees a reset, the kernel queue drains, and we count `shed_count`;
3. reopen the reserve;
4. **pause**: unwatch the listener and arm a back-off timer (100 ms); when it fires, watch again and try.

Related accept rules (EC-N06): `EAGAIN` ends the burst quietly (queue empty); `ECONNABORTED` means that client left already, skip it and continue; `EINTR` is retried; anything else is counted and also pauses, never spins. A burst is capped (`max_accepts_per_event = 64`) and then re-armed with `rewatch`, so 200 simultaneous clients cannot monopolise the loop.

The back-off needs a timer; what if the timer table is full? A first version retried by calling itself, which recurses. The fix (commit `3e22818`): retry from a `post`ed task, so the loop paces it, guarded by an "epoch" token so a task queued before `stop()` or destruction becomes a no-op instead of touching a dead object.

## 8. Transport: partial writes and EPIPE
`write` may accept fewer bytes than offered (the kernel buffer is full): **partial writes are normal**, the caller keeps the rest and waits for `Write` readiness. `send` always passes `MSG_NOSIGNAL`, because writing to a closed peer otherwise raises `SIGPIPE`, whose default action *terminates the process* (EC-N03). Instead the caller gets `ConnectionClosed`. `read` returning 0 means the peer finished (EOF) and is a different result from `WouldBlock`; confusing the two is how servers hang or spin.

`loopback_test` pushes 1 B, 64 KiB and 4 MiB through a real listener, acceptor, two transports and the loop, exercising partial writes and `WouldBlock` for real.

## 9. What the tests prove
About 110 tests in `tests/net/`: `Fd`, errno table, syscalls and retry, the timer wheel (+ model test), poller contract tests on real fds (including "not reported again until new data"), the loop (ordering, budgets, stale events, throwing callbacks, `EINTR`), the acceptor with injected failures, and the loopback echo. Run the thread-sensitive ones under TSAN and repeated: `ctest --preset tsan` and `--gtest_repeat=50`. Flakiness here is information, not noise: the lost wake-up in section 5 only showed up that way.

## 10. Pitfalls
- **Reading `errno` later.** Any call in between may overwrite it. `SysResult` captures it at once.
- **Keying by fd number.** See section 4.
- **Syscalls under a lock, or locks in connection code.** Cross-thread work goes through `post` only.
- **Unbounded "just retry".** Every retry (EINTR, back-off) has a bound or a pause.
- **Forgetting `rewatch` after a budget cut** under edge-triggering: the connection stalls with data waiting.
- **A callback that throws.** Handlers are `noexcept` by contract; tasks are caught at the loop boundary.

## 11. Exercises
1. Why is a level-triggered loop more forgiving to a handler that reads only 4 KiB per event? What does it cost? *(It is told again; but it wakes for every readiness and cannot tell "still pending" from "new".)*
2. Fd 12 is closed and reopened by a new client within one iteration. Which two values in the token differ, and where is it compared? *(Generation, in `dispatch`.)*
3. In `request_wake`, why is the eventfd write after the unlock, and why is that still safe? *(`wake_requested_` is already true, so the loop is guaranteed to look at the queue; a later write just collapses.)*
4. Make the wake-up bug come back: replace the `bool` by an atomic cleared outside the lock and run the stress test with `--gtest_repeat=200` under TSAN. How many repeats until it hangs?
5. A listener gets 10,000 connection attempts per second at `EMFILE`. With a 100 ms back-off, roughly how many are shed per cycle, and what limits that? *(Up to `max_accepts_per_event` = 64 per shed; the rest wait in the kernel backlog or are refused by it.)*
