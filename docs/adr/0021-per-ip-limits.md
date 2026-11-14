# ADR-0021: Per-IP connection and connection-rate limits

- Status: **Accepted** (2026-11-11; the owner asked to continue to the next milestone, M10; the limits are REQUIREMENTS section 4 rows 115 and 116, threats T2 and T4 in SECURITY_DESIGN)
- Date: 2026-11-11
- Deciders: project owner, maintainer assistant

## Context
A single client address can open every connection the server allows (global cap `max_connections`, FR-S1) or hammer the upgrade path: each valid upgrade costs a SHA-1, a handler `on_open` (for the C API and PHP: an event and a host round trip) and, with permessage-deflate, tens of kilobytes. REQUIREMENTS section 4 therefore asks for two more limits, both "429 / close": **100 connections per IP** and **50 handshakes per second per IP**. SECURITY_DESIGN T2 (slowloris) and T4 (connection flood) name "per-IP caps" as the control. Nothing implements them yet; the global cap and the `EMFILE` reserve descriptor exist (M5/M6).

Forces:
- Connections live on several loops (ADR-0014), but the accept decision is made on loop 0. A connection ends on *its own* loop, so a per-IP count is written from several threads.
- Refusing must cost almost nothing: the whole point is to survive a flood.
- Every table needs a bound with a defined overflow action (hard rule). An attacker controls the source address, so the number of distinct keys is attacker-chosen.
- "Locks in connection/protocol code" are forbidden (CLAUDE.md). The limit table is server-composition state, like the `live_` atomic and the host bridge's handle table.

## Options considered
1. **Per-loop tables, no sharing.** No locks. But a client that is spread over four loops by round-robin gets four times the limit, and "per IP" would silently mean "per IP per loop". Rejected: wrong semantics.
2. **Account on loop 0 only; ending connections post a "released" message to loop 0.** One thread owns the table, no mutex. But the post can fail (bounded queue, EC-N14) and a lost release leaks a slot for ever, which locks the address out. Rejected: a failure mode that is both silent and permanent.
3. **One mutex-guarded table (`IpLimits`) shared by the accept path and the ending connections** (this ADR). One lock operation per accept and one per close, held for a single hash-map operation and never across a call out. Cost at the target of 20k handshakes/s: about 40k uncontended lock operations/s, negligible. Simple, exact, and a failed release cannot happen.
4. **Sharded atomics or a lock-free map.** Faster under contention, which is not where this server's time goes (the table is touched twice per connection life, not per message). More code to get wrong, for no measured gain. Not now; revisit if `wsrv_load` shows the mutex.

## Decision

### 1. What is limited
- `max_connections_per_ip` (default **100**, `0` = off): live connections from one address key. Over it: refuse.
- `max_new_connections_per_ip_per_second` (default **50**, `0` = off): a token bucket per address key, capacity = the rate (a burst of one second's worth), one token per accepted connection. Over it: refuse.

The rate is counted **at accept**, as the rate of *new connections* per address. A WebSocket connection performs exactly one handshake, so this is the handshake rate of REQUIREMENTS section 4; counting at accept (before any byte is read) keeps the refusal cheap and keeps the check on loop 0, out of connection code. A connection that never finishes its handshake still costs a token; that is deliberately strict.

### 2. The address key (EC-N18)
`IpKey` is what "the same client" means:
- IPv4: the 4 bytes.
- IPv4-mapped IPv6 (`::ffff:a.b.c.d`): the same key as the IPv4 address, so a dual-stack listener cannot be used to get two budgets.
- Other IPv6: the **/64 prefix** (first 8 bytes). A subscriber normally owns at least a /64, so counting single addresses would let one machine rotate through 2^64 of them.
Behind a reverse proxy or NAT every user shares one key; the limits are configurable and `0` switches them off (documented in OPERATIONS; real client addresses via the PROXY protocol are EC-N21, not part of this ADR).

### 3. The table is bounded, and fails closed
`max_tracked = 2 * max_connections` entries (each live connection holds at most one entry; the other half is slack for entries that only carry a drained bucket). A new key when the table is full triggers one sweep that removes entries with no connection and a full bucket; if still full the connection is refused (`TableFull`): under an address-spraying attack the server sheds, it does not grow. An entry is erased as soon as its last connection ends and its bucket is full again, so an idle server holds no state. Each entry is about 40 bytes (key, count, bucket, timestamp) plus the map node.

The map's hash is seeded per server from the OS random source: addresses are attacker-chosen, an unseeded hash would let them be picked to collide and make every operation under the lock linear.

### 4. Arithmetic
Integers only. The bucket holds milli-tokens (`uint64`); refill is `elapsed_ms * rate` with `elapsed` clamped to one second first (a full bucket after a second of silence), so no multiplication can overflow for any configured rate (rate is capped at 1 000 000/s). The clock is the monotonic `IClock`; a backwards step is treated as zero elapsed time.

### 5. Wiring
- `IpLimits` (`src/server/ip_limits.{hpp,cpp}`): `admit(peer) -> Admission`, `release(peer)`. `Admission` is `Admitted | TooManyConnections | RateExceeded | TableFull`. Every `Admitted` is paired with exactly one `release`.
- `Server::on_accept` (loop 0): after the global cap, `admit`. On refusal the descriptor gets a prebuilt 429 (one non-blocking `send`, no allocation, `Retry-After: 1`) and is closed. A client that already sent its request may see a reset instead of the 429 (closing with unread input); the response is best effort and the close is the guarantee. The global cap keeps its silent close (REQUIREMENTS: "refuse accept").
- `release` runs where the connection is erased (`LoopShard::on_removed` receives the peer from `ConnectionTable`), where an adopt or hand-off fails, and nowhere else.
- Config: `ServerConfig::{max_connections_per_ip, max_new_connections_per_ip_per_second}`, range-checked in `validate()`; the same two numbers are keys of the embedded/`wsrvd` text configuration, parsed there and range-checked once in `build()` (as for deflate).

### 6. Not in this ADR
Per-IP limits on *established* traffic (data-frame rate is per connection, M4), the metrics for refusals (the next M10 slice counts them by reason), the `RLIMIT_NOFILE` check at start (own slice), PROXY protocol.

## Consequences
+ One client address can no longer take the whole connection budget or flood the upgrade path; both limits are on by default.
+ Refusal is one `send` and one `close`; no connection object, no read, no timer.
+ The table is bounded and seeded; an idle server holds no per-IP state.
− One mutex shared by the accept path and every ending connection. Held for one map operation. Revisit (option 4) only if a profile shows it.
− Defaults of 100 and 50/s can hit legitimate users behind one NAT or proxy; the knobs exist and the 429 carries `Retry-After`.
− The 429 is best effort for clients that sent their request before the close.
- Revisit trigger: `wsrv_load` showing lock contention, or a deployment behind a proxy that needs real client addresses (then PROXY protocol, EC-N21).

## Slices
1. This ADR; `IpLimits` with unit tests (cap, bucket, keys, bound, threads); config fields and validation.
2. Server wiring (accept, release, 429), integration tests on one and several loops.
3. Embedded/`wsrvd` keys, OPERATIONS, lesson, edge-case rows.
