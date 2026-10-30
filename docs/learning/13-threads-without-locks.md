# Lesson 13 — Threads without locks

## Goal
Run the server on several threads and let *other* threads (a database callback, a timer, later the PHP runtime) send to a connection, without a single lock inside `Connection` or `ProtocolSession`. After this lesson you can explain what is shared between loops and what is not, why a `SessionHandle` holds an id and never a pointer, where the only mutexes are and why each is safe, and how a shutdown spans N loops.

The title is a slogan with a precise meaning: **no lock in connection or protocol code** (CLAUDE.md hard rule). Two small mutexes exist at the edges (section 4 and 5). Knowing exactly where they are, and that nothing else locks, is the point of the lesson.

## 1. The problem
Lesson 11 ran everything on one thread, which made it simple: a connection is only ever touched by its loop, so no two pieces of code can race on it. Two features break that.

1. **Many loops** (FR-S1, FR-S5): one thread cannot use all cores.
2. **Foreign threads** (FR-S3): application code that is *not* a loop wants to say "send this to that client". A pointer to the connection is useless to it, because the connection may be destroyed at any moment on its own thread.

The usual answer is to lock every connection. That is slow, deadlock-prone and spreads synchronisation through the code that is hardest to reason about. Our answer is the reverse: keep every piece of mutable state owned by exactly one thread, and let threads talk by *sending messages* (closures) to the owner.

## 2. Shared-nothing shards
`LoopShard` (`src/server/loop_shard.hpp`) owns everything one loop thread needs: the `EventLoop`, the `LoopHost`, the `ConnectionTable`, the shared read buffer and the handle link. A connection never touches another shard.

What the shards **do** share (ADR-0014 section 2) is short and each item is safe for a stated reason:

| Shared | Why it is safe |
|---|---|
| `ServerConfig`, `HandshakeNegotiator` | immutable after construction |
| `IHandlerFactory` | documented as callable from several loops at once; the user's code must be thread-safe |
| `ILogger`, `IClock` | contract says thread-safe |
| three `std::atomic` counters: live connections, next id, rejected | atomics; each has one writer role (below) |

If a new member of `LoopShard` needs to be read by two threads, that is a design change and an ADR, not a quiet edit.

## 3. Handing a new connection to another loop
Loop 0 runs on the thread that called `Server::run()`, owns the listener and accepts. For each client it picks the next loop round-robin (`next_shard_++ % loops`). If that is loop 0 it adopts the descriptor directly; otherwise it **posts** a task to the chosen loop and that loop builds the `Connection` on its own thread.

The hand-off task must be copyable (`std::function`), but a file descriptor must have one owner, so the `Fd` travels inside a `shared_ptr`. If the target loop is already gone, the task is destroyed unrun and the `Fd` closes with it: no leak, no extra code.

The global connection cap (FR-S1) is one atomic: loop 0 checks `live >= max_connections` and increments it **before** handing over; the owning shard decrements when the connection is destroyed. Only loop 0 ever increments, so the check cannot be overtaken between "look" and "add". Connection ids come from one counter, also incremented only by loop 0, so they are process-unique and never reused (EC-N12).

Why round-robin and not `SO_REUSEPORT`? See ADR-0014 section 1: the kernel's hashing is uneven for few clients, and every loop would need the same port (awkward with port 0).

## 4. `post()`: the one door into a loop
Every cross-thread interaction goes through `EventLoop::post(task)`:

```text
any thread:   post(task) --> [bounded queue, mutex] --> write eventfd
loop thread:  epoll_wait wakes on the eventfd --> pop up to N tasks --> run them
```

- The queue is **bounded** (`max_queued_tasks`, default 4096). When it is full `post` returns `BufferFull` immediately (EC-N14): a flood of posts from many threads cannot grow memory without limit, and the producer learns about it.
- The loop runs at most `max_tasks_per_iteration` tasks per turn, so a flood cannot starve socket events either.
- A task posted *by* a task runs in the next iteration (the loop never spins on its own output).
- The mutex here protects the queue and nothing else; it is held for a push or a pop, never while a task runs. It lives in the net layer, outside `Connection` and `ProtocolSession`.

The wake-up has one classic trap: a producer pushes after the loop checked the queue but before it sleeps, and nobody writes the eventfd, so the task waits until some unrelated event arrives. `wake_requested_` is read and written **only under the same mutex** as the queue, so "a wake-up is already guaranteed" cannot disagree with the queue contents. Read the comment on `request_wake()` and then try to break it (exercise 3).

## 5. `SessionHandle`: an id, not a pointer
`Session::handle()` returns a copyable `SessionHandle` that may be kept and used from any thread (ADR-0014 section 4). It contains exactly:

```cpp
std::shared_ptr<LoopLink> link_;   // shared with the shard
ConnectionId id_;                  // a number
```

It never holds a pointer to the connection. `send_text(text)`:

1. copies the text,
2. asks the link to `post` a task that says "on your thread, look up id N and, if it is still alive, send this to it",
3. returns `Result<void>` meaning **accepted for delivery**, not delivered.

On the loop thread the `ConnectionTable` does the lookup. A connection that has closed since is simply not found and the task does nothing (EC-N13). There is nothing to dangle, whatever the timing: the worst case is a message to nobody.

Compare with lesson 12's `StreamLink`: the same idea (the long-lived object holds a clearable link, never the target), but `StreamLink` is loop-thread-only, so a plain pointer cleared by the connection is enough. A handle crosses threads, so its link needs a mutex.

### What `LoopLink` protects
`LoopLink` holds `{mutex, EventLoop*, ISessionDirectory*}`. Two threads can disagree about whether the loop still exists: a sender about to call `post`, and the shard being destroyed. The mutex settles it:

- `submit()` locks, checks the pointers are non-null, calls `post`, unlocks. The lock is held for **one bounded queue push**.
- The shard calls `close()` first of all when it is destroyed: lock, set both pointers to null, unlock. After that every `submit()` returns `Closing`.

Why is it safe to run a posted task, if the shard could vanish? Because a posted task only runs while the loop is running, which is only while the directory exists. Tasks still queued when the loop is destroyed are discarded with it, never executed. Destruction order therefore matters and is part of the design: link closed first, then the table, with everything a connection touches while dying declared before the table (see the comment on `LoopShard`).

This is the **only** lock in `src/server/`. Two mutexes in total, both at the edge, both held for one queue operation.

## 6. Ordering and failure, honestly
- Sends from **one** thread to **one** connection arrive in order (a FIFO queue, one consumer). Across threads there is no order, and the API does not pretend otherwise.
- `Closing` from a handle means the server or loop is gone; `BufferFull` means the loop's task queue was full; `ResourceExhausted` is an allocation failure. None of them says anything about the connection's own outbound queue: that decision is made later, on the loop thread, by the usual slow-consumer policy. A handle send that the connection refuses is dropped and logged, and under the default policy the connection is closed with 1008. This keeps one policy in one place.
- A handle send costs a copy and a task. That is fine for notifications; fan-out to thousands of subscribers (M10) will need batching and is deliberately not solved here.

## 7. Shutdown across loops
`request_shutdown()` is itself a `post` to loop 0, so it works from a signal watcher or another thread. Loop 0 then posts `begin_shutdown` to every shard. Each shard sends its clients a 1001 Close and reports "drained" through an atomic counter when its last connection is gone (`on_drained` runs on that shard's thread). When all shards are drained, or the grace timer fires and a force is posted to all, loop 0 stops the worker loops and joins their threads, then `run()` returns.

Signals are handled on loop 0 only. `install_signal_handlers` blocks them on the calling thread *before* the worker threads are started, and a new thread inherits its creator's signal mask, so workers can never be the thread the kernel picks.

## 8. Tests, and what each proves
- `SessionHandleTest.*`: ordered delivery from another thread; commands for a session that is gone are dropped; a closed link refuses with `Closing`; a handle outlives the link owner; **`ClosingTheLinkWhileThreadsSendIsRaceFree`**, which is meant to run under TSan.
- `SmallQueueTest.AFullTaskQueueIsReportedAsBufferFull` (EC-N14).
- `ServerHandle.*` (real sockets): a handle kept from `on_open` sends from another thread; a handle used after the connection and then the whole server are gone is harmless.
- `MultiThreadStress.ChurnPushesAndAShutdownInTheMiddleOfTraffic`: many connections opening and closing on several loops while foreign threads push through handles, ending with a shutdown mid-traffic. It lives in `tests/integration/` and is part of the TSan run.
- `wsrv_load` (`tests/bench`): a throughput benchmark and a soak driver. The 1 h soak recorded in `docs/PERFORMANCE.md` finished with 0 failures, a flat descriptor count and no memory trend.

## 9. What the benchmark does and does not say
Throughput went 1.0 : 1.5 : 1.5 : 2.4 for 1, 2, 4 and 8 loops (`docs/PERFORMANCE.md`). The shape is not a clean doubling, and the flat spot at 4 loops is **not explained yet**. The client threads share the same hybrid-core machine as the server, so the numbers show that more loops help and that nothing serialises on a lock, but they are not a claim about scaling on dedicated hardware. Writing down what we do not know is part of the result.

## 10. Pitfalls
- **Keeping `Session&`** (or a pointer into it) in another thread: it is loop-thread-only. Keep a `SessionHandle`.
- **Blocking in a handler** (a database call, a sleep): it stops the whole loop and every connection on it. Hand the work to your own thread and come back through a handle.
- **A thread-unsafe `IHandlerFactory`, logger or clock** with `threads > 1`: the factory and logger are called from several loops at once.
- **Reading "accepted" as "delivered"**: a handle `Ok` means queued for the loop. The connection may close before the task runs.
- **Adding a lock "just here"** in `Connection` or `ProtocolSession`: do not. Post a task to the owning loop instead; that is what this design is for.
- **Holding the `LoopLink` mutex across anything but one `post`**: it would turn a bounded critical section into an unbounded one.

## 11. Exercises
1. List every member of `LoopShard` and mark which ones another thread may touch. Why is the answer "only the link, and only through its mutex"?
2. Make `SessionHandle` hold a raw `Connection*` instead of an id. Write the two-thread interleaving that crashes. Which existing test would catch it, and under which sanitizer?
3. In `EventLoop::post`, move the write to the eventfd *outside* the mutex but keep `wake_requested_` as is. Is there a lost-wake-up interleaving? (Read the comment on `request_wake()` first.)
4. Why does `LoopShard` call `LoopLink::close()` before destroying the table, and what would a sender see if the order were reversed?
5. Why does `on_accept` increment the live counter *before* handing the descriptor to a worker? Which bug appears if it incremented after?
6. Sketch what a least-loaded distribution policy would need to read from each shard, and say why that read must not be a plain `size()` of the table.
