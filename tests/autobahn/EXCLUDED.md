# Autobahn: what is excluded, what is raised, and why

Run with `tests/autobahn/run.sh` (full) or `tests/autobahn/run.sh --quick`. Reports land in
`build/autobahn-reports/`. The gate is: no `FAILED` verdict (`summarize.py`); every `NON-STRICT`
verdict is explained below.

## Excluded cases

None. Cases 12.* and 13.* (permessage-deflate, RFC 7692) were excluded until M9 and have run since 2026-11-11:
`run.sh` starts the echo example with `--deflate`, which offers the extension with the library defaults
(`DeflateOptions{}`, including the decompression-bomb ratio of 100). The other cases never offer the
extension, so they are unaffected.

No case is excluded because it "fails": a failing case is a bug to fix at its root, never a check to loosen.

## Limits raised for the harness only

The echo example's `--autobahn` flag raises these, and nothing else. They stay below the hard
ceilings in `src/protocol/session.hpp`; the library defaults are untouched (FR-F6, FR-F10).

| Limit | Default | `--autobahn` | Cases that need it |
|-------|---------|--------------|--------------------|
| `parser.max_frame_payload` | 1 MiB | 20 MiB | 9.1.*, 9.2.* send one frame of up to 16 MiB |
| `assembler.max_message_size` | 1 MiB | 20 MiB | same |
| `assembler.max_fragments` | 1024 | 1 048 576 | 9.3.*, 9.4.* send a 4 MiB message as 65 536 fragments of 64 bytes |
| `outbound.max_queued_bytes` | 1 MiB | 64 MiB | the echo of a 16 MiB message is queued whole |
| `max_data_frames_per_second` | 10 000 | 2 000 000 | 9.7.*, 9.8.*, 10.1.1 send bursts of small frames |

Checked against the case descriptions of the 2026-11-11 reports: 9.1.* and 9.2.* send one message of up to 16 MiB,
9.3.* and 9.4.* a 4 MiB message in 64-byte fragments (65 536 frames), 9.5.* and 9.6.* 1 MiB in one frame written in
64-byte chops (so they need no fragment limit), 9.7.* and 9.8.* 1000 small messages. The rate limit above was not
re-derived and may be higher than the suite needs; lowering it is a trial, not an assumption.

The decompression-bomb ratio is **not** raised: the most compressible traffic the suite sends is about 22.6:1
(131 072 000 bytes of payload in 5 794 858 bytes on the wire, averaged over the 1000 messages of cases 12.1.10 and later), a fifth of the default
limit of 100, and all 216 deflate cases pass at the default.

The suite is not a browser, so the harness also passes `--allow-missing-origin` (a missing Origin
is accepted; `allow_any_origin` stays off).

## Status

First full run: 2026-11-02, `tests/autobahn/run.sh` (optimized build, 301 cases, Autobahn|Testsuite 25.10.1).
Verdicts: **588 OK, 7 NON-STRICT, 6 INFORMATIONAL, 1 FAILED** (the report counts each case's behaviour and its
closing behaviour). Cases 12.* and 13.* were the excluded permessage-deflate ones.

**2.10 (fixed 2026-11-04).** Ten Pings back to back used to get one Pong because the outbound scheduler had a
single Pong slot (allowed by RFC 6455 section 5.5.3, but Autobahn checks for a Pong per Ping anyway). The scheduler
now queues up to 16 Pongs (about 2 KiB at worst per connection) and only beyond that replaces the last one with the
newest (decision (b) of the first report; EC-C02). Second full run: **589 OK, 7 NON-STRICT, 6 INFORMATIONAL, 0 FAILED**.

**permessage-deflate (2026-11-11, M9).** Cases 12.* and 13.* are 216 cases of 1000 compressed messages each (16 bytes
to 128 KiB, sent whole or auto-fragmented to 256 bytes up to 32 KiB) under different client offers (default, and
`server_no_context_takeover` / `server_max_window_bits` variants), run against `echo --deflate`. Full run (517
cases): **1021 OK, 7 NON-STRICT (the same seven), 6 INFORMATIONAL, 0 FAILED**; the 216 deflate cases are all OK/OK.
A second run of only 12.* and 13.* with the decompression-bomb ratio at the library default (100) gave 432 OK and
nothing else, which is why the harness does not raise it. The server answers with the parameters the negotiation
table allows: for the default offer `server_max_window_bits=12` (the policy default), for an offer of 9 (13.7.*) `9`,
and `server_no_context_takeover; client_no_context_takeover` in both.

**Over TLS (2026-11-04; with deflate 2026-11-11).** `tests/autobahn/run.sh --wss` runs the same cases against the echo
example served with `--tls-cert/--tls-key` (self-signed certificate; the suite's client does not verify it): 301 cases gave
**589 OK, 7 NON-STRICT (the same seven), 6 INFORMATIONAL, 0 FAILED**; with deflate on (517 cases) **1021 OK, 7 NON-STRICT
(the same seven), 6 INFORMATIONAL, 0 FAILED**. TLS adds no protocol difference. The grade of the TLS settings is a
separate check, `tests/tls/run_testssl.sh` (testssl.sh: Overall Grade A+, ADR-0019 amendment 6).

A run that tested nothing now fails the gate: with an unreachable server the suite writes an empty report and exits 0,
and `summarize.py` used to pass it (found when the first wss attempt hit a build without TLS).

**NON-STRICT (7): 3.2, 3.3, 4.1.3, 4.1.4, 4.2.3, 4.2.4, 5.15.** All have the same shape: a valid message is
sent, then a violation (RSV bit set, reserved opcode, or a continuation with nothing to continue). The suite
accepts both "the echo of the first message arrives, then the connection fails with 1002" and "the connection
fails with 1002 at once". We fail the connection immediately and drop the unsent echo, which is the second,
also accepted, outcome (RFC 6455 section 7.1.7: fail the connection). The close code is 1002 in all seven.

**Through the C consumer (2026-11-21, M11 exit).** `tests/autobahn/run.sh --capi` (and `--capi --wss`) runs the same 517 cases
against `examples/echo_c`, a C99 program that uses only `wsrv.h` (agent names `wsrv-c`, `wsrv-c-wss`). Results of the final
runs: plain **1020 OK, 8 NON-STRICT, 6 INFORMATIONAL, 0 FAILED**; wss **1019 OK, 9 NON-STRICT, 6 INFORMATIONAL, 0 FAILED**
(an earlier wss run gave the same totals as the second). The harness limits that the C++ example sets through structs are set
through text keys (`max_message_size`, `max_frame_payload`, `max_fragments`, `max_data_frames_per_second`,
`event_queue_bytes`; the first three except `max_message_size` are the new keys of ADR-0016 A1). The same seven cases are NON-STRICT as for the C++
server. One to two more cases are NON-STRICT on any given run and **which ones changes between runs**: 4.2.5 (plain),
3.4 and 4.1.5 (wss), 4.2.5 and 4.1.5 (the first wss run). They have the same shape as the seven: "send a small message, then a
violation" (3.4: RSV = 4; 4.1.5: reserved data opcode 7 with payload; 4.2.5: a reserved control opcode, from the suite's 4.2.* group), they end
with close code 1002, and the suite's verdict text is "Actual events match at least one expected". The difference from the C++
server is timing: the echo of the first message goes through the host (event queue, poll, send), so it more often loses the
race against the connection failing on the violation that follows, and we then drop the unsent echo (RFC 6455 section 7.1.7),
the same accepted outcome as the seven. Neither outcome is a FAILED. This is a stated cause from the case texts and the
timing, not a traced one; to make it deterministic, the host would have to be allowed to flush its echo before the failure,
which the library does not promise: it fails the connection at once.

**Through the PHP binding (2026-11-22, M12).** `tests/autobahn/run.sh --php [--wss]` runs the same 517 cases against
`bindings/php/examples/embedded_echo.php`: PHP 8.3 loading the release `libwsrv.so` through FFI (`Wsrv\Embedded\Server`,
ADR-0025), with the same harness limits as the C consumer set through the same text keys (agent names `wsrv-php`,
`wsrv-php-wss`). Plain: **1021 OK, 7 NON-STRICT, 6 INFORMATIONAL, 0 FAILED**; wss: **1021 OK, 7 NON-STRICT, 6 INFORMATIONAL,
0 FAILED**. The seven NON-STRICT cases are exactly the C++ server's (3.2, 3.3, 4.1.3, 4.1.4, 4.2.3, 4.2.4, 5.15), with the
same explanation. The PHP echo handler catches `ConnectionClosed`: a message right before a protocol violation arrives at the
handler after the library has failed the connection, and the send then throws; the first run of this suite ended after 39
cases because that exception left `run()` and stopped the server (the example now treats it as normal; lesson 37).

**Re-run on the 1.0.0 sources (2026-11-26, develop at `1e547c7`).** All six runs again, after the wake batching (ADR-0035) and the
read trim (ADR-0036) changed the paths a message takes: `run.sh`, `--wss`, `--capi`, `--capi --wss`, `--php`, `--php --wss`,
each with the 517 cases and deflate offered. **0 FAILED in all six.** The C++ server, plain and wss, and the C and PHP consumers
over plain gave **1021 OK, the same seven NON-STRICT, 6 INFORMATIONAL**. Both C and PHP over wss gave **1020 OK, 8 NON-STRICT**: the
seven and one more from the "small message, then a violation" group (4.2.5 for C, 3.4 for PHP), the timing difference described
above for the C consumer; PHP loads the same `libwsrv.so`, so the same explanation holds.
