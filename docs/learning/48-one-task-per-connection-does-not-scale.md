# Lesson 48 — One task per connection does not scale: chunking work for many connections

## Goal
After this lesson you can explain why a bounded queue sized for "commands" breaks when the number of commands grows with the number of connections, how a broadcast and a mass close hit that limit, and how posting work in chunks (ADR-0034) removes it without making any queue unbounded.

## The problem
Every command for a connection travels as one task in the owning loop's task queue. That queue is bounded on purpose (4096 slots; a quarter is kept for control tasks, so a bulk task may use 3072): a bounded queue is how a loop says "slow down" instead of eating all memory.

Two paths posted **one task per connection**:

1. **A broadcast.** `Broker::publish` sent to each subscriber with its own task. One loop could therefore accept at most 3072 recipients of one publish. The rest were refused. The benchmark on one loop thread: a 5000-subscriber room received 61% of every broadcast.
2. **Closing every connection of a lost application.** After the worker died, the daemon closed each connection it knew, one task each, and ignored the errors ("Closing means gone"). A full queue answers `BufferFull`, which does **not** mean gone. With 5000 connections on one loop, 904 stayed open for good, with no application behind them.

Neither bug could happen while the server held at most 4095 connections. Fixing that ceiling (ADR-0016 A2) made both reachable. This is the pattern to remember: *when you raise one limit, ask which smaller limit was hiding behind it.*

## Design
**Chunk the work, keep the bound.** A queue slot is the unit of back-pressure, so spend slots on *batches*, not on single connections:

- `LoopLink::submit_bulk_many(ids, command)` posts **one** task that runs the command on up to 256 connections of that loop.
- `send_to_many(handles, message)` groups the recipients by loop, posts chunks of 256, and returns a per-recipient report: accepted, refused, and permanently gone.
- A chunk is accepted or refused **whole**. That keeps a recipient's messages in publish order, because all chunks of one publish are posted before the next publish starts.
- Memory stays bounded: 3072 slots x 256 ids x 8 bytes = 6 MiB per loop, worst case, and the message itself is shared, not copied.

**Retry what was only refused.** `Relay::close_stale` now returns how many closes were refused with `BufferFull` and keeps them; the daemon asks again every 25 ms. Any other error (the connection is gone) is forgotten. The set is bounded by `max_connections`.

Why not just raise `max_queued_tasks`? It moves the cliff and keeps the per-recipient cost (a mutex and a wake-up per connection). Why not one task carrying every recipient? A burst of publishes would then hold `publishes x subscribers` ids with no bound.

## Walkthrough
1. Read `kBulkChunk` and `BulkSendReport` in `src/server/session_handle.hpp`.
2. `LoopLink::submit_bulk_many` copies the ids **before** taking the lock (the copy allocates) and posts one lambda that calls `ISessionDirectory::run` for each id. A missing connection is dropped there, as for a single send.
3. `send_to_many` (same file's `.cpp`): `group_by_loop` then `post_batch`. Each helper is small on purpose, so it stays inside the clang-tidy complexity limit.
4. `Broker::publish` rotates the start of the recipient list, then calls `send_to_many` once. The rotation now moves by one chunk, the unit a loop accepts or refuses: a step of one recipient would leave the far end of a big room refused almost every time.
5. `Relay::close_stale` / `IpcServer::close_stale_connections`: a refused close leaves the id in `stale_`, counted; the timer retries only the ones still refused.

## Tests
- A loop with a 4-slot queue (bulk lane of 3) and 50 subscribers: before, 3 accepted; after, 50.
- Five chunks into a lane of three: exactly three chunks accepted, the counts are per recipient, nothing is reported "gone".
- Two publishes reach every connection in order. A closed link reports every recipient refused **and** gone.
- The fairness test: four chunks, a lane of three, four publishes: every subscriber is refused exactly once.
- Mutation check: set `kBulkChunk` to 1 (the old cost) and the headline test fails. A test that cannot fail proves nothing.
- Relay: closes refused with `BufferFull` are counted and retried; `Closing` is forgotten and not retried; only refused ones are asked again.
- End to end on the built image, one loop thread: 5000 and 10 000 subscribers receive 100%; 5000 idle connections are all closed after the worker is killed.

## Pitfalls
- "Refused" and "gone" are different answers to a send. Only the second is final.
- A fairness rotation must move by the unit that is refused (a chunk), not by one element.
- A chunk refused as a whole counts all its recipients as refused: under real overload up to 255 more recipients than strictly needed are reported refused.
- Do not retry a refused broadcast: a late, out-of-order delivery is worse than a counted refusal. Closes are different: they must happen, and order does not matter.

## Exercises
1. Why is the chunk size 256 and not 4096? What does it change in the worst-case memory and in how long one task holds the loop thread?
2. A subscriber disconnects while its id sits in a queued chunk. Which line makes this safe, and what does it cost?
3. Add a metric for chunks refused. Would it replace `wsrv_broker_deliveries_total{result="refused"}` or sit next to it, and why?
4. The close retry polls every 25 ms. Design a version without polling that wakes up when a loop has room again. What new thread-safety problem does it create?
