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

The harness is written and the echo example builds and serves. The first full run is still to be
done: the Autobahn image could not be pulled from the machine where this was written (the Docker
Hub CDN host did not resolve). Record the first report here when it exists.
