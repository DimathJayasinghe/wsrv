# ADR-0043: A message that can never be sent is refused, not dropped

- Status: **Accepted** (2026-11-27). Decided by the maintainer assistant on the advisor's recommendation, under the owner's standing instruction that design questions go to the advisor and that a failure that can happen is fixed before it happens. Found by an independent review of the Echo operations branch (ADR-0042 A2).
- Date: 2026-11-27
- Deciders: maintainer assistant (advisor's recommendation); the owner is told in the pull request

## Context
Each connection has an outbound queue of `max_queued_bytes`, and every queued message costs its payload plus 16 bytes (`kMessageOverhead`). `OutboundScheduler::can_enqueue` therefore refuses, with `BufferFull` ("can never fit"), a message larger than `max_queued_bytes - 16` however empty the queue is.

That refusal happens **on the loop thread, after the command was accepted**. `SessionHandle::send_text` posts a task and returns OK; the task calls `Session::send_text` and discards its result. `Connection::apply_send_policy` acts only on `Backpressure` (queue full for now), not on `BufferFull` (never). So a message that could never be sent was dropped silently, the IPC `STATUS` said OK ("accepted for sending, not delivered", ADR-0016 section 5), and an HTTP publish of such a payload answered `accepted`. Nothing was logged and no counter moved.

How it showed. The Echo guide (ADR-0042) recommended `--max-message-size=16384`. `set_max_message_size` sets the queue to `2 x max_message_size`, so one browser could be sent at most 32 752 bytes. A presence join whose member list was larger got **no answer**: the browser's page stayed "subscribing" for ever while the gateway counted the member and told the others about it. The review reproduced it against a real `wsrvd`: with 1 000-byte `user_info` members 1 to 31 were answered and member 32 was not. The gateway's clean failure for oversize lists (ADR-0042 A2 item 4) only covers lists over `ipc_max_frame`, which the guide had set far higher.

Two more facts found while fixing it:
- With `max_message_size` **unset** the queue was `max_queued_bytes = 1 MiB`, exactly the size of the biggest accepted message, whereas `--max-message-size=1048576` (the same value) gave 2 MiB. A browser could send a message of 1 MiB - 15 bytes or more and an echo of it could never be queued. Two spellings of the same configuration behaved differently.
- The same drop applied to a broadcast: `broker_max_payload_bytes` (default 64 KiB, up to 1 MiB) is not tied to the queue.

## Decision
A send or publish that can never fit is refused **where the caller can be told**, and the limits that promise otherwise are lowered to what is true. No protocol change.

1. **`largest_message(OutboundConfig)`** (`outbound_scheduler.hpp`) is the exact size `can_enqueue` accepts on an empty queue: `max_queued_bytes - 16`. One definition, tested at its boundary.
2. **Direct sends.** `LoopLink` takes `max_message_bytes` (set from `largest_message` in `LoopShard`) and refuses a larger `send_text`/`send_binary` on the caller's thread with `InvalidArgument`, the same code and reasoning as "larger than the whole pending-send budget": retrying cannot help. Over IPC this is `STATUS` `INVALID_ARGUMENT`; in the C API, `WSRV_ERR_INVALID_ARGUMENT` (documented in `wsrv.h`).
3. **Broadcasts and publishes.** `EmbeddedConfig::broker_config()` returns the broker bounds with `max_payload_bytes` capped to `largest_message`, whatever order the keys were set in; the publish endpoint's body bound follows it, so an oversize publish gets HTTP 413 (its body is at least as large as its payload).
4. **The IPC frame limit.** `wsrvd` lowers `ipc_max_frame` (and the default `max_message_size + 64 KiB`) to `largest_message + 14`, 14 being what a `SEND_*` frame's length counts besides its payload, but never below what the biggest inbound message needs (`max_message_size + 18`). An operator-set value that is lowered is logged once (`ipc_max_frame_lowered`); a default is lowered silently. A worker's client checks its frames against `max_frame` from `HELLO_OK`, so it now refuses the message *before writing it*, and the Echo gateway's existing `MessageTooLarge` path (join fails alone, `pusher:subscription_error`) covers both limits with no gateway change for sizes. Lowering loses nothing: no frame above that size can carry a message the daemon would send.
5. **The default queue is twice `max_message_size`**, the rule `set_max_message_size` already applied, now also in the `EmbeddedConfig` constructor. Trade-off, stated: the default per-connection backlog goes from 1 MiB to 2 MiB, so the slow-consumer close (1008) comes later and the theoretical sum of buffers at the defaults goes from about 20 GiB to about 30 GiB; the global budget of ADR-0040 (default 1 GiB, soft) is what bounds the sum, and every deployment that already set `max_message_size` ran at 2x. `ServerConfig`'s own default is left alone (embedders that build a `ServerConfig` directly decide their own queue).
6. **The gateway and a refused answer.** `Link::send` returns the request id of the send. For the message that tells a browser it joined, a `STATUS` that is not OK (and not `NO_SUCH_CONN`) closes that browser with 1011 and counts it in `daemon_refused`: its page would otherwise wait for ever for an answer while the room counts it as a member (`BACKPRESSURE` from a full send budget or task queue is the other way to lose that message). `pusher-js` reconnects after any close but 4000-4099 and joins again; `closed()` takes the member out. No rollback after the fact.

## Not decided here
- The `BACKPRESSURE` variant for a message that fits the queue but not its free space is the slow-consumer policy working as designed (close 1008 or drop); it is documented, not changed.
- A `PUBLISH` frame has a longer head than a `SEND`, so an IPC publish near the cap can get `MessageTooLarge` at the worker slightly below the broker's bound. That is an honest refusal; the frame is not widened for it, because that would reopen the window for `SEND`.
- When `max_message_size` is so small that `largest_message` is below `max_message_size + 18` (under about 20 bytes: nonsense) the inbound floor wins.

## Consequences
+ A message that cannot be sent is an error where it is issued, in the C API, over IPC and over HTTP, not silence.
+ The Echo guide's sizing rule is true, and `wsrvd` with no flags behaves like `--max-message-size=1048576`.
+ A 1.0 deployment with `--max-message-size=N` and a larger `--ipc-max-frame` keeps starting; the frame limit is lowered and logged.
- The default per-connection memory ceiling doubles (above).
- A worker that sent a message between `largest_message` and the old frame limit used to succeed silently (and lose the message); it now gets an error. That is the point, and the CHANGELOG says so.

## Tests
`MessageCapTest.*` (session_handle_test), `OutboundScheduler.LargestMessageIs...`, `EmbeddedConfig.The*Outbound*`/`TheDefaultQueue*`/`ThePublishBodyBound*`, `DaemonConfig.*Lowered*`, `PusherGatewayTest` (join answer refused), and `php_pusher_interop` (a real daemon at `--max-message-size=16384`: the announced frame limit and the member that no longer gets silence; it failed against the previous binary with "no frame within 10 seconds"). Each new line was broken once by hand and a test failed (mutation check by hand).
