# Lesson 44 — Broker commands over the IPC link

## Goal
After this lesson you can explain how a PHP worker joins a browser to a room through `wsrvd`, why the new frames are gated by the protocol minor version, why a bound is `LIMIT` and not `BACKPRESSURE`, and why the daemon drops subscriptions the moment the application disconnects.

## The problem
The broker lives inside the daemon. The application is another process that only talks IPC (ADR-0017). To use rooms it needs three new commands: put one of my connections in a topic, take it out, send to a topic. Each must be as safe as `SEND_TEXT`: bounded, validated, answered with a status.

## 1. Three frames, same shape as the old ones
`SUBSCRIBE` (12), `UNSUBSCRIBE` (13) and `PUBLISH` (14) are length-prefixed canonical frames like the rest (docs/IPC_PROTOCOL.md). `SUBSCRIBE` and `UNSUBSCRIBE` carry `request, conn, topic` with the topic as the rest of the frame; `PUBLISH` needs the payload after the topic, so its topic has a `u16` length. Parsing is strict: wrong size or trailing bytes is `InvalidFrame`, and a connection id of 0 is refused. The fuzz target `fuzz_ipc_frame` already feeds any frame type to the decoder, so the new types are fuzzed without a new target.

## 2. Why a version gate
A daemon at minor 3 talks to an application that said minor 2 in its `HELLO`. That application cannot know frames 12 to 14, so if it sends one it is confused or hostile. The daemon treats it as a protocol error and closes the link; it does not guess. The rule is `peer_minor < kBrokerMinor`. Test: `AnApplicationThatSaidMinor2CannotKnowBrokerFramesAndIsCutOff` sets `hello.minor = 2` by hand, because the default `Hello` already says 3.

## 3. The relay checks first, then asks the backend
`Relay::execute` first refuses what the application has no right to do: request id 0, and a connection it was not told about (`NO_SUCH_CONN`, the same ownership rule as `SEND_*`; one application cannot touch another's connections). Only then does the backend run. The topic is never read from a browser; the application decides who joins what.

## 4. `LIMIT` is not `BACKPRESSURE`
`BACKPRESSURE` means "try again soon". A broker bound (too many topics, too many subscribers) will not clear by waiting; it clears when someone leaves. Giving it the same code would make clients retry in a loop. So `WSRV_ERR_LIMIT = 12` is new, mapped from the broker's `BufferFull`. The header's ABI minor is bumped later with the C ABI slice.

## 5. No orphans
When the application link drops, the connections stay for the restart grace period (they close with 1012 afterwards). But a connection nobody can talk to must not keep receiving room traffic, and it must not hold a slot against the global bound. So `application_lost` calls `drop_subscriptions` for every connection that application knew, at once. Test `WhenTheApplicationDisconnectsItsConnectionsLoseTheirSubscriptionsAtOnce` uses a 5 s grace and checks the drop arrives with no 1012 close yet.

## 6. Pitfalls we hit
- `PublishReport` lived in `server/broker.hpp`, and `wsrvd` may not include `server/`. The include checker caught it; the fix was moving the small struct to `base/`, not weakening the rule.
- clang-tidy limits (function size, cognitive complexity 15) forced splitting `write_body` (a `BodyWriter`) and `handle` (`dispatch` and `dispatch_broker`).
- A fake backend that records every command made old tests that expected "no commands" fail once new kinds existed; tests now ask `commands_of(kind)`.
- The PHP cdef drift test failed because `wsrv.h` gained `WSRV_ERR_LIMIT`; regenerate with `php bindings/php/tools/gen_cdef.php`.

## Exercises
1. Send `PUBLISH` with request id 0. Which layer refuses it and with what status?
2. Why must the lock order (bridge, then broker) hold in the daemon path too?
3. Add a fourth topic bound in the broker. Which three places change?
