# ADR-0024: Pub/sub broker (topics and rooms)

- Status: **Accepted** (2026-11-14; M10 slice 3; REQUIREMENTS FR-S7, ARCHITECTURE section 2 `Broker`)
- Date: 2026-11-14
- Deciders: project owner, maintainer assistant

## Context
FR-S7: "pub/sub topics and broadcast with per-subscriber backpressure isolation". ARCHITECTURE sketches `Broker` as "Topic -> subscribers (per-loop shards; cross-loop fan-out via `post`)". Everything needed to reach a connection from another thread already exists: `SessionHandle` posts a command to the connection's loop, bounded, and the command is dropped if the connection is gone (ADR-0014 section 4). What is missing is the registry (who listens to what), the rules that bound it, and a way to send one message to many connections without copying it once per subscriber.

Forces:
- Topic names come from peers (a chat client says "join room X"): they are attacker-chosen, so the registry must not be a hash table keyed by them, must bound its size, and must validate the name.
- A `SessionHandle` send is "accepted for delivery", never "delivered"; a connection that closed on a live loop silently drops the command. The broker therefore cannot learn that a subscriber died from a failed send: the subscription must be removed by the connection's end.
- One slow subscriber must not delay or drop messages for the others (the FR-S7 isolation), and a publish storm must not let memory grow without bound.
- No locks in connection/protocol code; the registry is composition state (like `IpLimits`).

## Decision

### 1. A thread-safe registry in the server module, built on `SessionHandle`
`Broker` (`src/server/broker.{hpp,cpp}`): `subscribe(handle, topic)`, `unsubscribe(id, topic)`, `unsubscribe_all(id)`, `publish_text/publish_binary(topic, payload, except)`, `subscriber_count`, `topic_count`. Any thread may call any of it. One mutex guards the tables and is held for table operations only, **never while sending**: `publish` copies the subscriber handles (cheap `shared_ptr` copies) under the lock, releases it, then sends. No per-loop shards: the fan-out is already per loop because each `SessionHandle::send` is one `post` to the subscriber's own loop, and the registry is touched twice per subscription life and once per publish, not per message per connection. Revisit (ARCHITECTURE's per-loop shards) only if a profile shows the mutex.

A **room** is a topic: join = subscribe, leave = unsubscribe, members = `subscriber_count`. No second concept.

### 2. Names, bounds, and the data structures
- Topic name: 1..`max_topic_length` (128) bytes of `[A-Za-z0-9._:/@-]` only: no spaces, no control characters, no UTF-8, no wildcards (a name is also written into logs and the chat protocol). Anything else is `InvalidArgument`.
- `std::map`, not a hash table, for topics (attacker-chosen keys: O(log n) whatever they are); subscribers keyed by the server-assigned `ConnectionId`.
- Bounds, each with a defined overflow action (`BufferFull`, the subscription is refused and nothing changes): `max_topics` (1024), `max_subscribers_per_topic` (10 000), `max_topics_per_connection` (64), `max_subscriptions` in total (100 000). The total bound is what keeps a forgotten unsubscribe from growing memory for ever.
- A topic with no subscriber is erased at once; publishing to an unknown topic delivers to nobody and creates nothing.
- `max_payload_bytes` (64 KiB, `InvalidArgument` above it): a publish is a message another peer's handler wrote, not a bulk channel.

### 3. One immutable copy for all recipients
`SharedMessage` (in `session_handle.hpp`) is an immutable payload plus its type, text validated as UTF-8 **once** when it is built. `SessionHandle::send(const SharedMessage&)` posts a command holding a `shared_ptr` to it: N subscribers share one allocation; each connection copies into its own bounded queue as for any send. Memory in flight is therefore bounded by the existing limits (the loop's task queue and every connection's `max_queued_bytes`), not by payload times subscribers.

### 4. Backpressure isolation
Each subscriber is its own post to its own loop and its own connection queue. If a loop's task queue is full the broker's send for *that subscriber* returns `BufferFull`; it is counted as refused and the others are unaffected. A connection whose queue is full is handled by its own slow-consumer policy on its loop (close 1008 or drop), exactly as for any send. `publish` returns a report (`subscribers`, `accepted`, `refused`); it never blocks and never retries. A `Closing` answer (the server is gone) removes the subscription.

### 5. Leak-proof lifetimes
`BrokerMembership` is an RAII object a handler holds as a member: it records the connection's id and calls `unsubscribe_all` when the handler is destroyed (the handler lives exactly as long as its connection). This is the documented way to use the broker; calling `unsubscribe_all` from `on_close` also works. Because a dead connection cannot be detected by a failed send, the global subscription bound (section 2) caps the damage of a handler that forgets.

### 6. Metrics
`wsrv_broker_publishes_total`, `wsrv_broker_deliveries_total{result="accepted|refused"}` (ADR-0023): the broker takes an optional `IMetrics*`, and the server exposes a shared, multi-writer bank for it (`Server::shared_metrics()`); counters are relaxed atomics, safe from any thread.

### 7. Not in this ADR
The C ABI (`wsrv_publish`, `wsrv_subscribe` in BINDINGS, "planned M10") and the PHP `Client::publish()/subscribe()`: they need a host-bridge design (topics owned by the host process, a stable ABI decision) and are a follow-up, not a quiet extension of this one. Wildcard topics, message history/replay, delivery acknowledgements, cross-process brokers (Redis-style), ordering across publishers (per publisher thread to one subscriber the order is preserved by `SessionHandle`; there is no global order).

## Consequences
+ FR-S7 for C++ applications: rooms and topics with bounded memory and per-subscriber isolation, built on what already existed.
+ One payload allocation per publish, however many subscribers.
- One mutex on the publish path; held briefly, never across a send.
- Delivery is "accepted for delivery": a subscriber's queue or loop may still refuse or drop (counted, never silent).
- A handler that does not use `BrokerMembership` (or `unsubscribe_all`) leaks subscriptions until the global bound refuses new ones.
- Not available to PHP / C hosts yet.

## Slices
1. (done) This ADR; `SharedMessage` + `SessionHandle::send`; `Broker`, `BrokerMembership` with unit tests; metrics (`Broker::use_metrics`, `Server::shared_metrics()`); server-level tests across three loops; OPERATIONS section 3.2, lesson 33.
2. The chat example (examples/chat), with the 1.0-rc documentation slice.
