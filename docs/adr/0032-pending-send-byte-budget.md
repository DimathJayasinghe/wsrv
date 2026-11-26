# ADR-0032: A byte budget for sends waiting for a loop

- Status: **Accepted** (2026-11-23; closes review item 4a of RELEASE.md section 3, "build before 1.0")
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant
- Amends: ADR-0014 section 4 (`SessionHandle`, `LoopLink`)

## Context
`SessionHandle::send_text` / `send_binary` copy the message into a task and post it to the owning loop (ADR-0014). The loop's task queue is bounded in **count** (`max_queued_tasks`, 4096) but not in **bytes**: 4096 waiting sends of 1 MiB each hold 4 GiB. The sender is the application's own code, not a peer, so this is not remotely exploitable; it is a way for a host bug or a burst (a worker that answers a slow client with large replies, an HTTP publish loop) to take the process down with out-of-memory instead of being told "slow down". Every other buffer in the server has a byte bound and a defined overflow action; this was the one that did not (second security review, item 4a, accepted for the release candidate "to be built before 1.0").

## Decision
- **A per-loop budget of payload bytes.** `LoopLink` owns a counter (`PendingBytes`, lock-free). `send_text` and `send_binary` reserve `size` bytes **before** copying, and the reservation is held by the posted task until the task has run, or is destroyed unrun (a refused post, a loop that goes away). The reservation is a reference-counted `SendLease`, because `std::function` needs a copyable closure.
- **Overflow action:** the budget used up ⇒ `Error{BufferFull}` ("accepted for delivery" was refused; wait and retry, the same as a full task queue). The C ABI maps it to `WSRV_ERR_BACKPRESSURE` and the daemon relays `BACKPRESSURE` to the worker. A payload larger than the **whole** budget ⇒ `Error{InvalidArgument}` (`WSRV_ERR_INVALID_ARGUMENT`): it can never be sent, and a retryable code would make a caller loop forever.
- **Configuration:** `max_pending_send_bytes`, per loop, default **64 MiB**, range 1 .. 1 GiB (`kMaxConfiguredQueue`), no unlimited value. It is an ordinary key: config file, `--max-pending-send-bytes`, `WSRVD_MAX_PENDING_SEND_BYTES`, the C `wsrv_config_set`. Total worst case is `threads × budget`.
- **Not charged:** broadcasts (`SessionHandle::send(const SharedMessage&)`, already on the bulk lane, one shared buffer for all subscribers, size bounded by the broker's payload limit); `close`, `resume_reads` and handshake decisions (tiny, and must always get through).
- **What it counts:** payload bytes, not the closure or queue overhead (a few dozen bytes per task, already bounded by the task count).

## Consequences
+ Every host-fed buffer now has a byte bound and a defined overflow action; the last open item of the security reviews that was marked "before 1.0" is closed.
+ No new lock and no allocation on the refusal path: one compare-and-swap.
− A message larger than the budget cannot be sent at all; set the budget above your largest message (the default is 64× the default 1 MiB message size).
− The budget is per loop, not per connection: one stuck connection's pending sends can use the budget of its loop. The slow-consumer policy and the connection's own queue (ADR-0013) still bound what a single connection holds once the loop runs the task.
