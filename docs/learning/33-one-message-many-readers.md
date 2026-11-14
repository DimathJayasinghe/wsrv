# Lesson 33 — One message, many readers (the pub/sub broker, M10 slice 3)

## Goal
After this lesson you can explain how a broadcast reaches subscribers on other loops without a lock in any connection code, why a publish must never send while holding the registry lock, why one slow subscriber cannot delay the rest, and how a connection's end removes its subscriptions even though a failed send cannot tell the broker the peer is gone.

## Background: what a room needs
A chat room is "everybody who joined gets what anybody says". Three things make that hard in this server: the subscribers live on different loops (threads) that never share state; the names ("room 17") come from peers, so they are attacker input; and any subscriber may be slow or dead. ADR-0024 decides how.

## 1. The registry is composition state, the delivery is not
`Broker` is a `std::map` from topic name to subscribers behind **one mutex**, owned by the application like `IpLimits`; no connection or protocol code touches it, so the rule "no locks in connection/protocol code" holds. The delivery path is the one that already existed: `SessionHandle::send` posts a command to the subscriber's own loop (bounded queue, ADR-0014). So fan-out is automatically per loop; the broker only has to know who to post to.

## 2. Never send under the lock
`publish` does three steps: lock, copy the subscriber handles into a vector (a handle is a `shared_ptr` and an id, so a copy is cheap), unlock; then send to each copy. If it sent while locked, every `subscribe` on every thread would wait behind the slowest `post`, and a handle method that ever took another lock would be a deadlock waiting to happen. The cost of copying is a small allocation per publish; the benefit is that the lock is held for a table lookup and nothing else.

## 3. One payload, N readers
`SharedMessage` holds the bytes in a `shared_ptr<const vector<byte>>`. Copying it is a reference count, so the command posted to each loop carries a pointer, not the payload, and a 64 KiB message to 1000 subscribers is one 64 KiB buffer plus 1000 small closures. Each connection then copies the bytes into its **own** bounded outbound queue on its loop thread (that is the existing `Session::send_text`), so the memory in flight is bounded by the loop queue sizes and per-connection limits, not by payload times subscribers. Text is checked as UTF-8 **once**, when the `SharedMessage` is built; a bad string sends nothing to anybody and the publisher learns why (`InvalidUtf8`).

## 4. Isolation is the absence of shared waiting
Subscriber A's loop queue is full: the send for A returns `BufferFull`, the report counts one refusal, and the loop continues with B. Subscriber C's connection has a full outbound queue: that is handled on C's loop by C's slow-consumer policy (close 1008, or drop). Nothing waits for anything. The report (`subscribers = accepted + refused`) and the `refused` counter make loss visible; there is no retry, because a retry loop is how one slow reader becomes everybody's latency.

## 5. A failed send cannot say "the peer left"
`SessionHandle::send` answers "accepted for delivery", and if the connection has gone while the loop lives, the command is dropped silently on the loop. The broker therefore **cannot** garbage-collect by watching send results (only `Closing`, "the whole server is gone", is permanent and is acted on). The answer is ownership: a handler holds a `BrokerMembership` member, a handler lives exactly as long as its connection, and the destructor calls `unsubscribe_all`. Whatever ended the connection (Close frame, reset, timeout, server stop), the handler is destroyed, so the subscription is removed. The server test `AConnectionThatEndsGivesItsSubscriptionBack...` ends a client with a plain TCP close and waits for the topic count to fall to zero.

Because a handler could still forget, the total number of subscriptions has its own bound; a leak then costs a bounded amount of memory and shows up as refused subscriptions, not as an outage.

## 6. Names are attacker input
A topic name is written into logs and, in a chat protocol, back to other peers. So it is `1..128` bytes of `[A-Za-z0-9._:/@-]`: no spaces, no control characters, no UTF-8 (nothing to normalise), no wildcards (nothing to expand). The table is an ordered `std::map`: a hash table keyed by attacker-chosen strings can be flooded into its worst case; a tree is O(log n) whatever the keys are. Publishing to a topic nobody listens to creates nothing, so a peer cannot fill the table by publishing. An emptied topic is erased at once.

## 7. Rollback on allocation failure
`add_locked` checks every bound first, then inserts into two tables. If the second insertion throws `bad_alloc`, the first must be undone, or a topic would list a subscriber the other table does not know. The code removes whatever was inserted and returns `ResourceExhausted`; the total counter is increased only after both inserts succeed. A mutation check showed the test that repeated a `subscribe` five times was needed: without the idempotence check the counter drifted upward and the total bound would have refused honest clients.

## 8. What the security review added
A reviewer found that the design above had one gap: the broadcast shares the loop's task queue with everything else. A publish to 10 000 subscribers could fill a loop's queue, and then a new client could not be handed to that loop, a handshake decision was lost and a graceful stop became a forced one. The fix is a **reserve**: broadcasts go through `post_bulk`, which refuses once the queue is three quarters full, so the last quarter is only for control tasks. It is the same idea as a reserved lane on a road: not faster, but the ambulance can pass. `publish` also starts at a rotating offset: when the loops cannot take every delivery, the refused ones are the tail of the order, and a fixed order would refuse the same subscribers for ever.

Two smaller lessons from the same review. A raw pointer to something another object owns (the broker's pointer to the server's counters) is a use-after-free waiting for a thread you did not think about; a `shared_ptr` costs one copy per publish and removes the question. And compression is per connection: "one allocation per publish" does not mean "one unit of work per publish" (ADR-0024 A2).

## Tests
`BrokerTest.*` (delivery, order, `except`, bounds, names, isolation with a deliberately tiny loop queue, a gone server, metrics, four threads under TSan), `SharedMessage.*`, `SessionHandleTest.*SharedMessage*`, `BrokerServer.*` (three loops, real sockets). Mutation checks that went red: sending to the publisher, ignoring `Closing`, dropping the total bound, dropping the idempotence check.

## Pitfalls
- Sending while holding the registry lock.
- Treating "send returned ok" as "delivered".
- A per-connection copy of the payload made on the publisher's thread (N allocations per publish).
- Counting a subscription twice when it is repeated.
- Using the topic name from a peer without a charset and length check.

## Exercises
1. `publish_text` holds the lock only to copy handles. What can go wrong if a subscriber unsubscribes between the copy and the send? Why is that acceptable?
2. Why is `max_subscriptions` needed when `BrokerMembership` already unsubscribes on destruction?
3. Change the registry to one shard per loop. What would then be harder: subscribing, publishing, or counting the total bound?
4. Why is the bulk limit three quarters and not "the queue minus one slot"? What would a flood of control tasks (not broadcasts) do instead?
5. Wildcard topics (`news.*`) would need a trie or a scan. Which of the three bounds in ADR-0024 section 2 would stop being enough?
