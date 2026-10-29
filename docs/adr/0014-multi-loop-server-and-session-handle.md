# ADR-0014: Multi-loop server and the cross-thread SessionHandle

- Status: Proposed
- Date: 2026-10-29
- Deciders: project owner (to accept)

## Context
M6 runs everything on one loop thread. M7 (FR-S3, PROJECT_PLAN) adds N loop threads and a way for *other* threads (a database callback, a timer, the PHP runtime) to send to a connection. Both break the rule that made M6 simple: "everything runs on the one thread". Two hard rules must survive (CLAUDE.md): no locks in connection or protocol code, and cross-thread work only through `EventLoop::post`. Requirements: FR-S1, FR-S3, FR-S5; edge cases EC-N13, EC-N14; threats T3, T16; risk "cross-thread lifetime bugs" (PROJECT_PLAN section 8).

## Decisions

### 1. One acceptor, N worker loops, hand-off by `post`
The server runs `threads` loops (`ServerConfig::threads`, default 1, ceiling 256). **Loop 0 runs on the thread that calls `Server::run()`**, owns the listener and the acceptor, *and* serves connections like any worker. Loops 1..N-1 run on threads the server starts. The acceptor assigns each new descriptor round-robin and `post`s it to the chosen loop (directly when it is loop 0); the worker builds the `Connection`. With `threads = 1` nothing is posted and behaviour is exactly M6.

Rejected: `SO_REUSEPORT` with one listener per loop. The kernel then balances by connection hash, which is uneven for few clients, needs every loop to bind the same port (awkward with port 0), and makes graceful shutdown a per-listener problem. It stays available later as an option (`TcpListener` already supports the flag). Rejected: a dedicated acceptor thread that serves nothing: it wastes a core at low load.

### 2. Everything per-loop lives in one `LoopShard`
The loop, host, connection table, scratch buffer and `ConnectionContext` move from `Server` into a `LoopShard`, created **on its own thread** (the loop checks its thread, ADR-0013). Nothing in a shard is shared; connections never touch another shard. Shared between shards, and only these: the immutable `ServerConfig`, the `HandshakeNegotiator` (const after construction), the `IHandlerFactory` (already documented as called concurrently from several loops), the logger and clock (must be thread-safe, as `ILogger` already requires), and three atomics: the live-connection count, the next connection id, the rejected counter.

### 3. Global connection cap and ids
`max_connections` is global (FR-S1): the acceptor increments an atomic counter before handing a descriptor over and refuses at the cap; a shard decrements it when a connection is destroyed. Each shard's `EventLoopConfig` is sized for the *whole* cap, not cap/N, because the distribution is not guaranteed even; memory for unused slots is small and bounded. Connection ids come from one atomic counter, only incremented on the accepting loop, so they stay process-unique and never reused (EC-N12).

### 4. `SessionHandle`: weak, by id, loop-posted
`Session&` stays loop-thread-only and must not be kept. `Session::handle()` returns a copyable `SessionHandle` that can be kept and used from any thread. It holds only `{shared_ptr<LoopLink>, ConnectionId}`; **never a pointer to the connection**. `send_text/send_binary/close` copy the data, `post` a task to the owning loop, and that task looks the id up in the shard's `ConnectionTable`: if the connection is gone, the task does nothing. A pointer therefore cannot dangle (EC-N13), whatever the thread timing.

`LoopLink` is the one small shared object, owned by the shard and by every handle: `{std::mutex, EventLoop*, bool alive}`. A sender locks it, checks `alive`, posts, unlocks; the shard locks it and clears `alive` before its loop is destroyed. The mutex lives here, in the cross-thread facade, never in `Connection` or `ProtocolSession`, and is held only for one `post` call (a bounded queue push). This is the only lock in the module and it is covered by TSan tests.

Results: a handle call returns `Result<void>` meaning "accepted for delivery": `Error{Closing}` when the shard is gone, `Error{BufferFull}` when the loop's task queue is full (EC-N14). Whether the connection's own outbound queue can take the message is decided later on the loop thread, by the same slow-consumer policy as for in-loop sends (ADR-0013 section 4); a message the queue refuses is dropped and logged, and under the default policy the connection is closed with 1008. Sends from one thread to one connection arrive in order (a FIFO task queue); between threads there is no order.

### 5. Shutdown across loops
`request_shutdown()` posts `begin_shutdown` to every shard. Loop 0 arms the grace timer; each shard reports "drained" through an atomic counter; when all are drained (or the grace expires and `force` is posted to all) the other loops are stopped and joined, then loop 0 returns from `run()`. A second request forces, as in M6. Signals are handled on loop 0 only (`install_signal_handlers` blocks them on the calling thread *before* worker threads start, so workers inherit the mask).

### 6. Out of scope here
`Broker`/pub-sub and per-IP caps (M10), the C ABI wrapping of handles (M11), thread pinning/NUMA. `MessageWriter` (FR-F12) is planned in M7 but is a separate slice with its own decisions.

## Consequences
+ Scaling comes from independent shards: no shared mutable connection state, so the TSan surface is small (the link, the three atomics, the posted tasks).
+ `threads = 1` is M6 unchanged, so the whole M6 test suite stays a regression guard.
+ A handle is safe to keep after the connection, the shard or even the server is gone.
- Round-robin ignores load: one shard can hold the slow connections. Acceptable for 1.0; a least-loaded policy can replace it behind the same seam.
- A handle send costs a copy and a task per message. Fan-out to many connections (M10) will batch; this ADR does not.
- `ILogger`, `IClock` and `IHandlerFactory` implementations supplied by users must be thread-safe once `threads > 1`; documented in `ServerBuilder`.
