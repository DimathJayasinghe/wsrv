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
