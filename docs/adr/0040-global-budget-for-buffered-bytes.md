# ADR-0040: A global budget for the bytes connections buffer

- Status: **Proposed** (2026-11-26; the owner accepts). Closes item (b) of RELEASE.md section 3 ("no global buffered-bytes budget"), makes SECURITY_DESIGN section 6 true, and delivers what ADR-0013 deferred ("the global buffered-bytes budget lands in M10").
- Date: 2026-11-26
- Deciders: project owner, maintainer assistant

## Context
Every connection is bounded on its own: a partial message waits in the assembler for at most `max_message_size` (1 MiB by default), and the outbound queue holds at most `max_queued_bytes` (1 MiB; when it is full the slow-consumer policy closes the connection). `max_connections` bounds how many connections exist (10 000 by default), and per-address limits bound how many one address may hold (ADR-0021).

Nothing bounds the **sum**. At the defaults a worst case is 10 000 x (1 MiB + 1 MiB) = about 20 GiB, far more than most hosts have. Two ways to get near it with little effort:
- Many connections that each send the first fragments of a 1 MiB message and then stop (the assembler keeps the bytes until the message ends or the idle timer fires). The attacker pays the bytes it sends, and per-address limits do not help when the clients share an address (a NAT, a trusted proxy, ADR-0029) or when there are many addresses.
- Many slow readers while the application broadcasts: every queue fills towards its own 1 MiB limit before the policy closes the connection.

The process then dies of an out-of-memory kill instead of refusing work, and every client is dropped, not only the new ones. RELEASE.md recorded this as "accepted for 1.0 with the sizing formula documented; a budget key is post-1.0", and the owner has since set the rule that a failure that can happen is fixed before it happens. SECURITY_DESIGN section 6 even states that the server "has global budgets: ... total buffered bytes (soft cap, shed new connections)": today that sentence is not true.

## Decision

### 1. What is counted
Per connection: **the bytes of a partial inbound message in the assembler, plus the bytes in the outbound queue** (`ProtocolSession::buffered_input_bytes()` and `buffered_output_bytes()`). These are the two buffers a peer can make grow.

The output side counts the queue **and** what is already framed in the wire buffer and not yet written. The first draft used the queue alone (`queued_output_bytes()`); the first connection test showed that it counts zero for a client that reads nothing, because the first message leaves the queue for the wire buffer as soon as the transport asks for output, and that is precisely the client the budget exists for. `OutboundScheduler::held_bytes()` is the sum, and both parts are bounded, so it cannot wrap.

Not counted, and why:
- **permessage-deflate state**: bounded per connection, allocated on first use, and a function of the number of connections (OPERATIONS section 1.1 gives the figure, about 96 KiB each at the defaults), which `max_connections` already bounds.
- The shared per-loop read buffer (one per loop, not per connection), the handshake head (at most 8 KiB, `http.max_total_bytes`), the bytes behind an upgrade waiting for the application (at most one read each, at most `max_pending_handshakes` of them), control frames waiting in their slots (a Ping, a Pong, a Close: at most 125 bytes of payload each; one that is already framed in the wire buffer is counted with the rest of it) and TLS records in flight (one record).
- Direct sends waiting for a loop: they have their own budget (ADR-0032).

### 2. How it is counted: charge by difference, refund by lifetime
Each connection owns a small `BufferLedger` holding the number of bytes it has charged so far (`charged_`) and a pointer to one shared `std::atomic<std::size_t>`. At the two places where a connection's buffers can change, after it handled a read and after it wrote (`settle()`), it asks the session for the current total and calls `ledger.set(total)`, which adds the **difference** to the shared counter (relaxed). The ledger's destructor subtracts exactly `charged_`.

Why this shape:
- **It cannot drift.** The refund is the connection's own record of what it charged, not a recomputation, and it runs from a destructor, so every way a connection can end (a clean close, an error, an abort, a force shutdown, a server torn down with live clients) gives the bytes back. A counter that is off by one never comes back, and a budget that only grows turns a defence into a self-inflicted outage: the tests assert the gauge returns to exactly 0.
- **No cost on the common path.** A connection that buffers nothing (idle, or an echo whose messages fit in one read) has `charged_ == 0` and `set(0)` does no atomic operation, so loops do not contend on one cache line.
- **No per-allocation hooks, no walk over all connections** (that would be O(connections) per tick), no lock (ARCHITECTURE: no locks in connection code). It copies the pattern of the global connection cap: one atomic owned by the `Server`, handed to the shards in `ShardShared`.

### 3. What happens when the budget is used up
It is a **soft cap on admission**. In `Server::on_accept`, on loop 0, next to the `max_connections` check: when the counter is at or above `max_buffered_bytes`, the new client gets the prebuilt `503` with `Retry-After: 1` (one non-blocking send on a fresh socket, as for a full per-address table) and is closed; the metric `wsrv_connections_total{result="rejected_buffer_budget"}` counts it and a `buffer_budget` warn event is logged on the 1st, 2nd, 4th, 8th, ... refusal. **Existing connections are not touched.** A burst can overshoot between two checks by at most what the connections already admitted can still add, and that is bounded by their own limits. Closing live connections to get back under the cap (a hard cap) is a different decision with a different cost (it drops people who did nothing wrong) and is left as a follow-up.

The gauge `wsrv_buffered_bytes` shows the current sum.

### 4. Configuration and the default
One key, `max_buffered_bytes` (config file, `--max-buffered-bytes`, `WSRVD_MAX_BUFFERED_BYTES`, the C `wsrv_config_set`): default **1 GiB**, range 1 byte .. 1 TiB, **no unlimited value** (the rule of ADR-0032).

The arithmetic for the default. A healthy connection buffers close to nothing: an idle one 0, an echo 0, a 10 KiB message in flight a few KiB. To reach 1 GiB at the shipped limits the server needs at least 512 connections that each hold two full 1 MiB buffers at the same moment (and a full outbound queue is closed by the slow-consumer policy), or all 10 000 holding about 105 KiB each at the same instant. That is an overloaded server, not a legitimate 1.0 load, so the default cannot shed what 1.0 served; the 503 is the right answer in that state.

The default protects a host with several GiB of memory. A smaller one must set the key: **about half of the memory limit** you give the process, after subtracting the deflate state (OPERATIONS section 1.1). The reference Kubernetes manifest limits memory to 1 GiB, so it sets `WSRVD_MAX_BUFFERED_BYTES` to 256 MiB, and so does the systemd example.

### 5. What this does not do
- It is not a limit on resident memory. It counts what a peer can make a connection buffer, not allocator overhead, deflate state or the transport.
- It does not close existing connections (follow-up, see section 3).
- There is no per-loop share. A single global counter is the simpler contract, and loops are not isolated from each other in any other budget either.

## Options considered
1. **Document the sizing formula and do nothing** (the 1.0 position). Honest, but the failure is an out-of-memory kill of the whole process, and the formula is the operator's arithmetic, not a control.
2. **Sample every connection's total on a timer.** Simple, but O(connections) per tick, and a stale answer exactly when a burst is happening.
3. **Charge on every allocation and free** (hooks in the assembler and the scheduler). Exact, but it puts `server/` accounting into `protocol/`, which must stay free of it, and touches the hot path on every frame.
4. **A ledger that charges the difference at two points and refunds from its destructor (this ADR).**
5. **A hard cap that also closes connections.** Left as a follow-up.

## Consequences
- **The C ABI becomes 1.1** (`WSRV_ABI_VERSION`): a configuration key is an addition, and ADR-0016 A4 says a minor adds keys. No symbol changes. The PHP binding keeps requiring 1.0 (it works on a 1.0 library; only a host that sets the new key needs 1.1, and a 1.0 library answers that key with the usual unknown-key error).
+ SECURITY_DESIGN section 6 and RELEASE.md section 3(b) become true, and the operator has a gauge and a counter to alert on.
+ A server under a buffering attack, or under a slow-consumer storm, refuses new work with a clear 503 and keeps serving the connections it has, instead of being killed.
- A new refusal path in a minor version (only above 1 GiB buffered at the defaults), and a key every operator with a memory limit below about 2 GiB should set.
- The soft cap can be exceeded by the in-flight burst; it is a brake, not a wall.

## Slices
1. This ADR; the ledger, the session accessor and their unit tests.
2. The wiring (`Server`, `ShardShared`, `Connection`), the config key in all its forms, the metric and the gauge, with the server-level tests (a multi-loop test that the cap sheds and that the gauge returns to 0).
3. Documentation (OPERATIONS, SECURITY_DESIGN, RELEASE, ADR-0013 note, FRAGMENTATION, PROTOCOL_EDGE_CASES), the reference manifest and systemd example, lesson 53.
