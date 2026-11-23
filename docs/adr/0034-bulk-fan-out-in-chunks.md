# ADR-0034: Work for many connections is posted in chunks, and refused closes are retried

- Status: **Accepted** (2026-11-23; found by the 1.0 benchmark and its audit)
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant
- Amends: ADR-0014 section 4 (`LoopLink`), ADR-0028 (broker delivery), ADR-0010 (the sidecar's restart grace)

## Context
Every command for a connection travels as one task in the owning loop's bounded task queue (4096 slots; the last quarter is kept for control tasks, so a *bulk* task may use at most 3072). Two paths post **one task per connection**:

1. **A broadcast.** `Broker::publish` called `SessionHandle::send` for every subscriber. One loop could therefore take at most 3072 recipients of one publish while the loop was not draining. The rest were refused: counted (`refused`, the metric), reported to the publisher, and rotated so the same subscribers are not always the losers, but not delivered. The 1.0 benchmark on one loop thread: a 5000-subscriber room received 61% of every broadcast (3072 of 5001 per publish); with the default two threads the cliff sits at about 6000.
2. **Closing every connection of a lost application.** `Relay::close_stale` called `close` for every connection the application knew and ignored every error ("Closing = gone"). A refusal for a full queue (`BufferFull`) is not "gone". Checked on one loop thread with 5000 idle connections and the worker killed: 4096 were closed after the restart grace, **904 stayed open for good**, with no application behind them.

Both were unreachable while the server held at most 4095 connections (ADR-0016 A2 fixed that ceiling, which made these two reachable). The same class: a bounded queue sized for the number of commands, used with a number of commands that grows with the number of connections.

## Decision
**A. A broadcast takes one queue slot per chunk of recipients, not per recipient.**
- `LoopLink::submit_bulk_many(ids, command)` posts one bulk task that runs the command on every listed connection on the loop thread (the same `ISessionDirectory::run` as before, so a missing connection is dropped, and a connection with a full outbound queue handles its own overflow, exactly as for a single send).
- `send_to_many(handles, message)` groups the recipients by their loop and posts them in chunks of **256 ids**. The result is per recipient: how many were accepted, how many refused (a refused chunk refuses all its ids), and which connections are permanently gone (the loop is closed): the broker unsubscribes those.
- Recipient order is kept (the broker's rotation start still applies), and all chunks of one publish are posted before the next publish begins, so one connection still sees its messages in publish order.
- Worst case memory of queued broadcast tasks: 3072 bulk slots x 256 ids x 8 bytes = 6 MiB per loop, plus the message, which is shared and not copied. A loop that is behind refuses a chunk (`BufferFull`) and the publisher sees the refused count, as before.

**B. Closes the loops refuse are kept and retried.**
- `Relay::close_stale` returns how many closes were refused with `BufferFull` and keeps them; any other error means the connection is gone and is forgotten.
- `IpcServer` retries those every 25 ms until none are left. The set is bounded by the stale set, which is bounded by `max_connections`.

## Rejected
- **Raise `max_queued_tasks` or the bulk share.** Moves the cliff and keeps the per-connection cost (one mutex and one wake per recipient); ADR-0016 A2 rejected the same idea for the event queue.
- **One task per loop carrying every recipient.** Simple, but a burst of publishes then holds `publishes x subscribers` ids with no bound.
- **Let a refused broadcast block the publisher until the loops drain.** The publisher is the IPC thread (or an HTTP handler); blocking it stalls every other command.
- **Retry refused broadcast recipients.** A delivery that arrives late and out of order is worse than a counted refusal, and the broker's contract (ADR-0028) is best effort with a report.

## Consequences
- + A broadcast to any room the broker accepts (`max_subscribers_per_topic`, default 10 000) is delivered in full on an idle loop, at any thread count; fewer posts and wake-ups per broadcast, so it is also faster.
- + No connection outlives the application behind it because of a full queue.
- + The accepted/refused counters keep their meaning (per recipient), so IPC 0.3 and `ClusterPublishResult` are unchanged.
- - A chunk is refused or accepted as a whole: under real overload up to 255 more recipients than strictly necessary can be reported refused for one publish.
- - A close retry runs on a timer; with a very large stale set it takes several rounds.

## Verification
- A loop with a tiny queue (`max_queued_tasks` 4, bulk lane 3) and 50 subscribers: before, 3 accepted; after, 50.
- A pre-filled queue refuses the chunk and the refused count equals the recipients.
- `Relay`: a backend that refuses closes with `BufferFull`, then recovers: every stale connection ends up closed; one that reports `Closing` is forgotten, not retried.
- End to end on the image with `WSRV_THREADS=1`: 5000 and 10 000 subscribers receive 100% of a broadcast; 5000 idle connections are all closed after the worker is killed.

## Result (measured 2026-11-23, image built from this change, one loop thread, 2 CPUs)
- A broadcast to 5000 subscribers: 100% delivered (it was 61%); to 10 000 subscribers: 100% delivered; `wsrv_broker_deliveries_total{result="refused"}` stayed 0 over 75 010 deliveries.
- 5000 idle connections, worker killed: all 5000 closed within the restart grace (`wsrv_connections_current` 5000 to 0); 904 stayed open before.
- Tests: `BulkTest.*` and `BulkAcrossLoops.*` (session_handle_test), the reworked `SmallQueueBrokerTest.*`, `RelayTest.*Refused*/*Retried*`, `IpcServerTest.A1012CloseTheLoopRefusesIsRetriedUntilItIsAccepted`. With the chunk size set to 1 (the old cost) the headline test fails, so the test does catch the bug.

