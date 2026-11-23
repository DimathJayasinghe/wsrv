# Lesson 43 — Rooms for a host that only has connection ids

## Goal
After this lesson you can explain why `HostBridge` holds its mutex across `Broker::subscribe`, what the lock order is, and how a subscription can never outlive its connection.

## The problem
The C++ broker (ADR-0024) wants a `SessionHandle`. A host written in PHP or driven over IPC only ever sees a number: the connection id. Someone must turn the id into a handle, and someone must undo the subscriptions when the connection ends, because a host cannot be trusted to remember (a forgotten unsubscribe would fill the broker's global bound and refuse everybody).

## 1. The bridge already knows id to handle
`HostBridge` keeps a map `id -> SessionHandle` so `send_text(id, ...)` works. `subscribe(id, topic)` looks the handle up in that map and gives it to the broker. A connection the bridge does not know (never existed, or already closed) is `Closing`, the same answer `send_text` gives.

## 2. The race, and why the lock is held across the call
Two threads: the loop thread runs `on_close` (it calls `remove(id)`), a host thread calls `subscribe(id)`. Without care: the host finds the handle, the loop removes the connection and its subscriptions, then the host adds the subscription. Nobody will ever remove it: a leak.

The fix is to make "find the handle and subscribe" one step that cannot interleave with `remove`: `subscribe` keeps the bridge mutex while it calls the broker, and `remove` takes the same mutex before it calls `unsubscribe_all`. There are only two outcomes: the subscription is complete before `remove` runs (so `unsubscribe_all` deletes it), or `find` fails and the host gets `Closing`. There is no third.

## 3. The price: a lock order
Now two mutexes are held at once, always in the same order: bridge mutex, then broker mutex. The broker never calls back into the bridge, so no cycle exists. The rule is written in `host_bridge.hpp`. The bridge mutex is still never held while sending a message: a publish goes straight to the broker, which copies the subscribers and unlocks before it posts anything.

## 4. Test
`SubscribingWhileTheConnectionClosesNeverLeaksASubscription` runs 40 rounds of a host thread subscribing 20 times while the client closes; afterwards the topic must have no subscribers. Remove the lock in `subscribe` and the test, run under ThreadSanitizer, is the one that would complain.

## 5. Bounds as config, never "off"
The five `broker_max_*` keys use the same `parse_limit` as the other limit keys: a number from 1 to a ceiling. `0` and anything above the ceiling are refused, so a typo cannot switch a guard off. The counters reach `/metrics` through the server's shared metrics, and a test reads them from the admin page: a claim in a slice needs a test that observes it.

## Exercises
1. Why is `unsubscribe` allowed to take the bridge mutex only briefly (through `find`) while `subscribe` holds it across the broker call?
2. What goes wrong if `remove` calls `unsubscribe_all` after releasing the bridge mutex?
3. Set `broker_max_topics_per_connection` to 1, subscribe twice, and read the error. Which `ErrorCode` is it, and why is it not "backpressure"?
