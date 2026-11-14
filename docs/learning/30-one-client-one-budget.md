# Lesson 30 — One client, one budget (per-IP limits, M10 slice 1)

## Goal
After this lesson you can explain why a server needs a limit *per client address* on top of a global cap; why "the same client" is not "the same string"; how a token bucket works with integers only; why a table that an attacker can fill must be bounded and seeded; and why the lock here is allowed although connection code may not take locks.

## Background: the problem
The global cap (`max_connections`) stops the server from running out of descriptors, but one address can still use all of it, and every valid upgrade costs a SHA-1, a handler `on_open` (for the C API and PHP a host round trip) and, with deflate, tens of kilobytes. REQUIREMENTS section 4 asks for 100 connections and 50 new connections per second per address, and SECURITY_DESIGN names them as the answer to T2 (slowloris) and T4 (connection flood).

## 1. Who is "one client"? (EC-N18)
Comparing address strings is wrong. `203.0.113.5`, `::ffff:203.0.113.5` (the same host seen through a dual-stack socket) and, for IPv6, any of the 2^64 addresses of one /64 are the same client in practice. `IpLimits::key_of` normalises: IPv4 by 4 bytes, IPv4-mapped IPv6 as the IPv4 address, other IPv6 by the first 8 bytes. The port is not part of the key (every connection has a different one). Tests: `IpLimits.TheSameClientIsRecognised…`, `AnIpv6ClientIsItsWholeSlash64`.

## 2. A token bucket in integers
A bucket holds up to `rate` tokens; each new connection takes one; time puts them back at `rate` per second. Floats would round and drift, so the bucket counts **milli-tokens** (1 token = 1000) and refills with `elapsed_ms * rate`. Two guards make the arithmetic safe whatever the configuration: `elapsed` is clamped to one second first (a full bucket after a second of silence, so the product cannot overflow even at the ceiling of 1 000 000/s), and a clock that goes backwards counts as zero elapsed time. Test: `AfterALongSilenceTheBucketIsFullNotOverflowing` advances the fake clock a year.

One more detail that tests found worth keeping: a **refused** attempt costs neither a slot nor a token, otherwise an attacker hammering a full address would also drain its bucket and keep it locked out after the connections end.

## 3. Why a mutex is fine here
CLAUDE.md forbids locks in connection and protocol code. This table is *server composition* state, like the live counter. The accept path (loop 0) adds, and each connection ends on its own loop, so the count is written from several threads. Options in ADR-0021: per-loop tables (wrong: four loops would give four times the limit), post "released" to loop 0 (a failed post leaks a slot for ever), one mutex (chosen: one lock per accept and one per close, a single map operation each, never held across a call out). A failed release cannot happen, which is the property that matters.

## 4. The table an attacker fills
The key is chosen by the attacker, so the number of keys is attacker-controlled. Rules applied: the table has a bound (`2 * max_connections`); an entry is erased the moment it carries no information (no connection, bucket full); when full, one sweep, then **refuse** (fail closed: under an address-spraying attack the server sheds load, it does not grow); the hash is seeded from the OS random source so chosen addresses cannot be made to collide and turn every operation under the lock into a linear scan.

## 5. Refusing cheaply
The decision is made at accept, before any byte is read, so the refusal is one non-blocking `send` of a prebuilt 429 and a `close`: no connection object, no read, no timer, no allocation. A client that had already sent its request may see a reset instead of the 429; the close is the guarantee.

## Tests
`IpLimits.*` (16, fake clock, no sockets, includes six threads under TSan), `IpLimitsServer.*` (real loopback sockets, one and three loops: the slot comes back from any loop, also for a connection that never finished its handshake), config tests for defaults and ceilings, the text keys through the embedded C++ API, the end-to-end 429 through `EmbeddedServer`, and the `wsrvd` flags and usage. Existing churn tests switch the limits off, because they open hundreds of connections from one address on purpose, and the Autobahn mode of the echo example does too.

## Pitfalls
- Counting at the point you can *reject*, but releasing in a different set of places than where you can *end*: every path that ends a connection (also a failed hand-off, a failed TLS wrapper, a handshake that never finished) must give the slot back, and an extra release must not grant more than the cap.
- Using the port or the full IPv6 address as the key.
- Testing the limit with the default configuration in unrelated tests: 100 connections from `127.0.0.1` is a normal test.
- Defaults that hurt behind a proxy: every user is one address. Document it, offer `0`.

## Exercises
1. A client opens 50 connections in one second and keeps them. What does the 51st attempt in the next second get, and why? Then what if it closes one first?
2. Why does `AClockThatGoesBackwardsGrantsNothing…` also advance time forward again by the same amount and still expect a refusal?
3. Add a metric that counts refusals by reason (`TooManyConnections`, `RateExceeded`, `TableFull`). Where is the counter incremented so that a refusal is counted exactly once? (Slice 2 does this.)
