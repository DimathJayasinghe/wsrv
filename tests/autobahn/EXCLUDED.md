# Autobahn: what is excluded, what is raised, and why

Run with `tests/autobahn/run.sh` (full) or `tests/autobahn/run.sh --quick`. Reports land in
`build/autobahn-reports/`. The gate is: no `FAILED` verdict (`summarize.py`); every `NON-STRICT`
verdict is explained below.

## Excluded cases

| Cases | Why | Until |
|-------|-----|-------|
| 12.*, 13.* | permessage-deflate (RFC 7692). The server does not negotiate it yet, so these cases cannot pass by design. | M9 |

Nothing else is excluded. In particular no case is excluded because it "fails": a failing case is
a bug to fix at its root, never a check to loosen.

## Limits raised for the harness only

The echo example's `--autobahn` flag raises these, and nothing else. They stay below the hard
ceilings in `src/protocol/session.hpp`; the library defaults are untouched (FR-F6, FR-F10).

| Limit | Default | `--autobahn` | Cases that need it |
|-------|---------|--------------|--------------------|
| `parser.max_frame_payload` | 1 MiB | 20 MiB | 9.1.*, 9.2.* send one frame of up to 16 MiB |
| `assembler.max_message_size` | 1 MiB | 20 MiB | same |
| `assembler.max_fragments` | 1024 | 1 048 576 | 9.3.*, 9.4.*, 9.5.*, 9.6.* chop a multi-MiB message into 64-byte frames |
| `outbound.max_queued_bytes` | 1 MiB | 64 MiB | the echo of a 16 MiB message is queued whole |
| `max_data_frames_per_second` | 10 000 | 2 000 000 | 9.7.*, 9.8.*, 10.1.1 send bursts of small frames |

The case sizes above are taken from the Autobahn case descriptions as remembered when this harness
was written; the first full run must confirm them, and this table is to be corrected from that
run's report.

The suite is not a browser, so the harness also passes `--allow-missing-origin` (a missing Origin
is accepted; `allow_any_origin` stays off).

## Status

First full run: 2026-11-02, `tests/autobahn/run.sh` (optimized build, 301 cases, Autobahn|Testsuite 25.10.1).
Verdicts: **588 OK, 7 NON-STRICT, 6 INFORMATIONAL, 1 FAILED** (the report counts each case's behaviour and its
closing behaviour). Cases 12.* and 13.* are the excluded permessage-deflate ones.

**FAILED: 2.10** (10 Pings sent back to back; the suite expects 10 Pongs and gets only the last one).
This is the documented design, not an accident: the outbound scheduler has one Pong slot and a newer Pong
replaces an unsent one, which RFC 6455 section 5.5.3 allows ("MAY elect to send a Pong frame for only the
most recent Ping frame") and EC-C02 records. Autobahn checks for a Pong per Ping anyway ("not required by the
Spec .. but we check for this behaviour anyway"). The gate says a failing case is a bug to fix at its root,
so this needs an owner decision: (a) keep one slot and record 2.10 as an accepted deviation here, or (b) queue
a small bounded number of Pongs (say 16; beyond that, coalesce to the latest), which makes 2.10 pass at the cost
of a slightly larger per-connection control queue. Not changed yet.

**NON-STRICT (7): 3.2, 3.3, 4.1.3, 4.1.4, 4.2.3, 4.2.4, 5.15.** All have the same shape: a valid message is
sent, then a violation (RSV bit set, reserved opcode, or a continuation with nothing to continue). The suite
accepts both "the echo of the first message arrives, then the connection fails with 1002" and "the connection
fails with 1002 at once". We fail the connection immediately and drop the unsent echo, which is the second,
also accepted, outcome (RFC 6455 section 7.1.7: fail the connection). The close code is 1002 in all seven.
