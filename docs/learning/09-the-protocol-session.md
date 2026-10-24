# Lesson 09 — The protocol session: Pong, the close handshake, timers

## Goal
Everything RFC 6455 asks of a connection *above* single frames and messages: answer Pings, run the closing handshake exactly once, notice dead or abusive peers, and send big messages without starving small control frames. Code: `src/protocol/session.*` and `src/protocol/outbound_scheduler.*`.

## 1. Why a sans-I/O session
The session never touches a socket or a clock. Bytes come in through `next_event(input)`, time comes in through `tick(now)`, bytes go out through `pending_output()`. Three consequences:
- **Deterministic**: the same bytes, calls and ticks give the same events and output. A test can run "wait 30 seconds" in microseconds, and the fuzzer can replay any crash exactly.
- **One owner per thread**: no locks. The connection layer (M6) owns it on the loop thread.
- **Views stay valid**: `next_event` hands back *one* event at a time, with views into your read buffer (frames are unmasked in place) or the assembler's buffer. A queue of events would outlive those views, so the API is pull-style, like `FrameParser::next`.

```text
 read():   while (auto e = session.next_event(input)) handle(*e);
 timer:    session.tick(now);     then  next_event(empty)  to collect Closed / errors
 app:      session.send_text(..) / close(..)
 writable: write(session.pending_output());  session.consume_output(n);
```

## 2. Three states
```text
 Open ──(we or the peer sends Close, or the peer breaks a rule)──► Closing ──(handshake done / timeout / EOF)──► Closed
```
`Closed` is terminal and is reported by exactly one `Closed` event. Nothing is read after the peer's Close and nothing is written after ours.

## 3. Ping, Pong
A Ping must be answered with a Pong carrying the same payload (RFC 6455 §5.5.3), *even in the middle of a fragmented message*. The assembler already delivers control frames without disturbing the message; the session just queues the Pong. If five Pings arrive before we get to write, only the **latest** Pong is kept (the RFC allows it, EC-C02). That is also our defence against a Ping flood: the Pong queue is one slot, so memory cannot grow (plus a rate limit, §6).

## 4. The close handshake, four ways
1. **Peer closes**: parse and validate the Close payload (1 byte, forbidden codes, bad UTF-8 reasons are protocol errors, Lesson 07), echo its code, report `Closed` once our echo is written. `clean = true`.
2. **We close** (`close(code, reason)`): queue Close, discard the half-received message, keep reading until the peer's Close arrives, dropping its data and Pings meanwhile (EC-C10). If it never answers, the **close timeout** ends it with code 1006.
3. **Both at once** (EC-C11): the second Close just completes the handshake; still one Close frame sent, one `Closed` event.
4. **The peer breaks a rule** (bad UTF-8, oversized frame, rate limit): queue *one* Close with the matching code, report `ProtocolError`, ignore the peer from then on, and report `Closed` (clean=false) when the Close is written. RFC 6455 §7.1.7 does not wait for the peer's reply here.

`Closed.code` is the peer's code if it sent one (1005 if its Close was empty), else the code we sent when we failed the connection, else **1006**: "abnormal", never sent on the wire, used when the TCP connection just died (EC-F23) or the peer never answered.

## 5. Outbound: what goes on the wire next
`OutboundScheduler` keeps data messages *unframed* in a bounded queue (1 MiB by default, plus 16 bytes charged per message so a flood of empty messages cannot grow it). When the wire buffer is empty it frames the next piece, in this order:
1. the Close, if queued (then nothing else, ever);
2. the pending Pong;
3. the pending Ping;
4. the next fragment of the oldest data message, at most `max_frame_payload` (64 KiB) of it.

So a 200 KiB message leaves as four frames (64 + 64 + 64 + 8 KiB: first `Binary FIN=0`, then `Continuation`s, the last with `FIN=1`), and a Ping or Pong queued meanwhile goes out **between** two of those frames, never inside one (EC-F32). The wire buffer holds one data frame plus control frames, so memory per connection is bounded by the frame size, not the message size. A full queue answers `Backpressure` ("try again after draining"); a message larger than the whole queue gets `BufferFull` ("never fits"). Nothing is queued on error.

## 6. Time: timers and rate limits
| What | Default | When it fires |
|---|---|---|
| idle timeout | 60 s | no *complete frame* received → close 1001 |
| heartbeat | 30 s / 10 s | quiet for 30 s → send Ping; no Pong 10 s later → close 1001 |
| incomplete message | 30 s | a fragmented message not finished → 1008 |
| close timeout | 5 s | Closing too long → `Closed` (1006, not clean) |
| control frames | 100 / s | more → 1008 |
| data frames | 10 000 / s | more → 1008 |

All durations of 0 (or rate 0) disable that rule. Boundaries are exact: nothing at `timeout - 1 ms`, fires at `timeout`; `limit` frames are fine, `limit + 1` is not. Rate windows are measured in ticks: the first `tick` at least one second after the window started resets the counters, so the connection layer must tick at least once a second.

*Slowloris* (a peer that trickles one frame byte by byte to hold the connection) is stopped by choosing "idle = no complete frame" rather than "no bytes": a test sends all but the last byte of a 100-byte frame, one byte per second, and the 10 s idle timer still closes the connection.

## 7. What the tests prove (and what they did not)
- `Session.*` (scripted peer), `SessionSend.*`, `SessionTimers.*`, `SessionRates.*`: 48 tests; `OutboundScheduler.*`: 15 tests. The test peer builds masked frames and parses server output with a *client-mode* `FrameParser`, so even the tests check our output is valid protocol.
- Mutation checks: dropping the "drop data after our Close" rule, answering Pings after our Close, ignoring input after a failure, never reporting `clean`, `>` instead of `>=` at a timeout, a Pong not clearing the heartbeat: each fails tests. One mutant survived on purpose: counting control frames at their *header* instead of their end differs only for control frames cut in half, which no test sends. We recorded it rather than inflate the suite.
- `fuzz_session` runs arbitrary scripts of "feed bytes / tick / send / close / write a few bytes" and asserts: at most one `Closed`, nothing after it, the state only moves forward, output is always valid frames with at most one Close frame and nothing after it, pending output stays bounded.
- Two review findings on this milestone, both fixed: an `assert` that wrapped a memory allocation (so memory pressure would abort the process: it is now an error path), and a `tick()` that stored the time but enforced nothing (it was a stub between commits; the timers and limits are now tested).

## 8. Pitfalls
- **Reporting `Closed` too early.** If we reported it when the Close was *queued*, the transport would be closed before the echo reached the peer. It waits until the Close is fully written.
- **Sending after Close.** Any frame after our Close is a protocol error on the peer's side. The scheduler drops unsent data when Close is queued and refuses new messages (`Error{Closing}`).
- **Trusting a timer you never tick.** Rate limits and timeouts only move when `tick()` is called; the connection layer owns that.
- **Time going backwards.** `tick` ignores earlier values: a bug in a caller's clock must not re-arm timers.

## 9. Exercises
1. A peer sends `Text FIN=0 "ab"`, then a Ping, then `Continuation FIN=1 "cd"`. List the events and the output. *(Events: Ping, then Message "abcd"; output: one Pong.)*
2. Why does a protocol failure not wait for the peer's Close, but a local `close()` does? *(After a violation we no longer trust the peer's bytes; after a normal close we want its acknowledgement, bounded by the close timeout.)*
3. With `max_frame_payload = 64 KiB`, how many frames carry a 130 KiB message, and where may a Pong appear? *(Three frames: 64, 64, 2 KiB; before the first, or between any two, never inside one.)*
4. Name two ways memory per connection stays bounded on the outbound side. *(Data queue bound; one slot each for Pong, Ping, Close; the wire buffer holds one frame.)*
5. Give an input where `clean` is true but we never wrote our echo. *(The peer sends Close and closes TCP immediately: `on_transport_closed` reports its code with `clean=true`, EC-C13.)*
