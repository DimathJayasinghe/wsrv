# ADR-0027: Topics and rooms through the C ABI (ABI 0.5)

- Status: **Proposed** (2026-11-22; the owner moves it to Accepted)
- Date: 2026-11-22
- Deciders: project owner, maintainer assistant

## Context
ADR-0024 gave C++ applications a thread-safe `Broker` (topics, rooms, one shared payload, per-subscriber isolation) and left the C ABI out (its section 7: "needs a host-bridge design"). Without it a PHP, Python or C host, which only sees `wsrv.h`, must keep its own room table (the PHP chat example does, with its own bounds) and send one message per member through the host's own thread. That is the work the broker exists to do: one `post` per subscriber on the subscriber's own loop, one payload allocation, bounded queues.

What makes the C ABI different from a C++ caller:
- The host knows connections only by `wsrv_conn_id`. The broker needs a `SessionHandle`. `HostBridge` already keeps `id -> SessionHandle` (that is how `wsrv_send_text` works), so the mapping exists.
- A C host cannot hold an RAII `BrokerMembership`. If subscriptions lived until the host remembered to remove them, a forgotten unsubscribe would leak until the global bound (ADR-0024 section 2) refused everyone. In C++ the handler's destructor does it; here the library must.
- Connection end and a host command run on different threads (the loop thread runs `on_close`, any host thread calls `wsrv_subscribe`). A subscribe that finds the handle just before the close removes it, and then adds the subscription after the close cleaned up, would leak a subscription for a connection that no longer exists.
- Topic names and payloads cross the boundary as bytes with explicit lengths, like every other string in `wsrv.h`.

## Options considered
1. **Broker in the library, owned by `EmbeddedServer`, driven by conn id** (this ADR). Reuses everything; the host cannot leak; costs nothing when switched off.
2. **A callback-based broker in the host.** Rejected by the core rule (ADR-0010, BINDINGS section 1): no host code on an I/O thread.
3. **Expose `SessionHandle` as an opaque C pointer** that the host passes to the broker. More surface, a lifetime the host must manage (free calls, thread rules) and nothing the conn id does not already give.

## Decision

### 1. Ownership and switch
`EmbeddedServer` owns one `Broker` when the config key `broker` is `true` (default `false`: no object, no memory, no cost). Its bounds are config keys, validated at `wsrv_server_start` like all others (`WSRV_ERR_INVALID_CONFIG` with the reason): `broker_max_topics`, `broker_max_subscribers_per_topic`, `broker_max_topics_per_connection`, `broker_max_subscriptions`, `broker_max_payload_bytes`; the defaults are `BrokerConfig`'s (1024, 10 000, 64, 100 000, 64 KiB) and each has a documented upper limit. The broker's counters join the server's shared metrics when the admin listener is on (ADR-0023, ADR-0024 section 6).

### 2. The functions (ABI 0.5, additive)
```c
wsrv_status wsrv_subscribe(wsrv_server *, wsrv_conn_id, const char *topic, size_t topic_len);
wsrv_status wsrv_unsubscribe(wsrv_server *, wsrv_conn_id, const char *topic, size_t topic_len);
wsrv_status wsrv_publish_text(wsrv_server *, const char *topic, size_t topic_len,
                              const char *text, size_t text_len, wsrv_conn_id except,
                              wsrv_publish_report *report);          /* report may be NULL */
wsrv_status wsrv_publish_binary(wsrv_server *, const char *topic, size_t topic_len,
                                const uint8_t *data, size_t data_len, wsrv_conn_id except,
                                wsrv_publish_report *report);
wsrv_status wsrv_topic_subscribers(wsrv_server *, const char *topic, size_t topic_len, size_t *count);

typedef struct wsrv_publish_report { size_t struct_size; size_t subscribers, accepted, refused; } wsrv_publish_report;
```
Any thread, like the other commands. Topic and payload are copied during the call; nothing the host passed is kept. `except` is the publisher in a chat (0 = nobody). `struct_size` is set by the host before the call, as for `wsrv_event`, so the struct can grow.

### 3. Statuses
One new value, `WSRV_ERR_LIMIT = 12`: a broker bound refused a subscription (topics, subscribers per topic, topics per connection, or the total). Retrying does not help until someone leaves, which is why it is not `WSRV_ERR_BACKPRESSURE` (a full queue, "try later"). The rest uses what exists: a topic name or payload the broker rejects is `WSRV_ERR_INVALID_ARGUMENT`; text that is not UTF-8 is `WSRV_ERR_INVALID_UTF8` (checked once, nothing sent); an unknown or ended connection is `WSRV_ERR_NO_SUCH_CONN`; a call with the broker off is `WSRV_ERR_INVALID_ARGUMENT` (wrong state); a forked child is `WSRV_ERR_WRONG_PROCESS`. A publish never fails because subscribers are slow: it returns `WSRV_OK` and the report says how many were accepted and refused (ADR-0024 section 4). `wsrv_status_name` knows the new name, and the header says an unknown non-zero status is a failure, so hosts built against 0.4 stay correct.

### 4. The library cleans up, in one place and without a race
`HostBridge::remove(id)` (the first thing `on_close` does) also calls `broker.unsubscribe_all(id)`. The host therefore cannot leak, and does not have to unsubscribe on a close event.

To close the race in the Context, `wsrv_subscribe` calls `broker.subscribe` **while holding the bridge's mutex**, after finding the handle. Lock order: bridge mutex, then broker mutex; the broker never calls into the bridge, and neither lock is held while sending (the bridge's mutex is already "one map operation, never across a call out"; a broker `subscribe` is one table operation). Either the subscription is complete before `remove` runs (and `unsubscribe_all` removes it), or `find` fails and the host gets `WSRV_ERR_NO_SUCH_CONN`. There is no third outcome. The lock order is stated in `host_bridge.hpp`, and a TSan stress test (hosts subscribing while connections close) is a slice-1 requirement.

### 5. What the host still owns
- **Policy.** The broker does not know who a connection is. ADR-0024 A4 applies unchanged: an application that serves strangers applies its own per-account or per-address quota before `wsrv_subscribe`, and checks that a peer may join a room before it joins it. `BINDINGS.md` and the PHP README say so in the same place as the functions.
- **Names.** A topic name that came from a peer is validated by the broker (charset, length), but "may this peer see this room" is the host's.
- **Order.** Per publisher thread, per subscriber, the order is kept; there is no global order (ADR-0024 section 7). Unchanged.

### 6. Bindings
The PHP embedded binding gets `Server::subscribe($conn, $topic)`, `unsubscribe`, `publish($topic, $data, $binary, $except)` (returning the report as an array) and `subscribers($topic)`, with `broker` and the bounds as ordinary config keys. `WSRV_ERR_LIMIT` becomes a `LimitReached` exception subclass of `WsrvException` (a caller acts differently: tell the user the room is full). The cdef is regenerated and its drift test covers the new functions. The chat example then drops its own room table for the broker.

### 7. Not in this ADR
- **The sidecar (`wsrvd`, IPC).** `Client::publish()/subscribe()` need IPC v0.3 frames and a daemon-side broker; they get their own amendment to ADR-0017 after this ADR is implemented, and reuse the same bounds. Until then, PHP applications behind the sidecar keep their own rooms.
- Wildcards, history, acknowledgements, cross-process brokers: as in ADR-0024.

## Slices
1. **C++ core, no C yet.** Config keys and validation; `EmbeddedServer` owns the broker; `HostBridge` subscribe/unsubscribe/publish by id, unsubscribe on remove, the lock order; unit tests for the bounds and the race (TSan stress test); metrics wired.
2. **The C ABI 0.5.** `wsrv.h`, the shim, the status, ABI version, symbol-list and header checks, a C consumer test, `echo_c` option `--rooms` kept small, `BINDINGS.md`; an `ABI_MINOR` of 5 in the PHP binding's check is slice 3.
3. **PHP.** cdef, `Server` methods, `LimitReached`, interop test on the real library, the chat example on the broker, README, lesson.

## Consequences
- C and PHP hosts get rooms with the same bounds and isolation as C++ applications, without writing a fan-out loop in the host language.
- One more locked path (subscribe) takes two mutexes in a fixed order; it is documented and stress-tested.
- The ABI grows by five functions, one struct and one status. All additive; ABI minor 0.5.
- Hosts that never set `broker` see no change at all.
- Revisit if a host needs per-subscriber filters (then a callback-free predicate language, in its own ADR) or if one mutex becomes the bottleneck at high publish rates (ADR-0024's own revisit trigger).
