#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn bench/compare/results/<label>.json into the Markdown tables of docs/BENCHMARKS.md.

    python3 bench/compare/report.py bench/compare/results/2026-11-23.json

Every figure is the median of the repeated runs; a run that lost connections or messages is flagged
instead of averaged away. Prints to stdout.
"""
from __future__ import annotations

import json
import statistics
import sys

ORDER = ["wsrv", "workerman", "swoole", "reverb"]
NAMES = {"wsrv": "wsrv (wsrvd + PHP worker)", "workerman": "Workerman 5 (event)", "swoole": "Swoole", "reverb": "Laravel Reverb"}


def med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def spread(values, digits=0, scale=1.0):
    """'lowest to highest' of the repeated runs, so the noise is visible next to the median."""
    values = [v for v in values if v is not None]
    if len(values) < 2:
        return ""
    return f" ({min(values) / scale:,.{digits}f} to {max(values) / scale:,.{digits}f})"


def fmt(v, digits=0, scale=1.0, unit=""):
    if v is None:
        return "n/a"
    return f"{v / scale:,.{digits}f}{unit}"


def flag(runs, *, bad=lambda r: "error" in r or "exit_code" in r):
    n = sum(1 for r in runs if bad(r))
    return f" ⚠ {n}/{len(runs)} runs had failures" if n else ""


def table(title, header, rows):
    if not rows:  # a scenario that was not run
        return
    print(f"\n#### {title}\n")
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join("---" for _ in header) + "|")
    for row in rows:
        print("| " + " | ".join(row) + " |")


def main(path: str) -> None:
    data = json.load(open(path))
    servers = [s for s in ORDER if s in data["servers"] and "error" not in data["servers"][s]]
    print(f"_Run `{data['label']}` on {data['docker']}; median of {data['settings']['repeats']} runs._")
    for s in data["servers"]:
        if "error" in data["servers"][s]:
            print(f"\n**{NAMES.get(s, s)} did not run:** {data['servers'][s]['error'][:200]}")

    # echo
    rows = []
    for conns, size in data["settings"]["echo"]:
        for s in servers:
            block = next((b for b in data["servers"][s].get("echo", []) if b["connections"] == conns), None)
            if not block:
                continue
            runs = block["runs"]
            rows.append([NAMES[s], str(conns), fmt(med(r.get("msgs_per_sec") for r in runs))
                         + spread([r.get("msgs_per_sec") for r in runs]),
                         fmt(med(r.get("p50_us") for r in runs), 2, 1000), fmt(med(r.get("p99_us") for r in runs), 2, 1000)
                         + flag(runs)])
    table("Echo (closed loop, 64-byte messages; Reverb is not here: the Pusher protocol has no echo)", ["Server", "Connections", "Messages/s", "p50 (ms)", "p99 (ms)"], rows)

    # idle
    rows = []
    for n in data["settings"]["idle"]:
        for s in servers:
            block = next((b for b in data["servers"][s].get("idle", []) if b["connections"] == n), None)
            if not block:
                continue
            res = block["result"]
            per = block.get("bytes_per_connection")
            rows.append([NAMES[s], str(n), str(res.get("established", "n/a")), fmt(block["baseline_bytes"], 0, 1e6, " MB"),
                         fmt(block["peak_bytes"], 0, 1e6, " MB"), fmt(per, 1, 1024, " KiB")
                         + (" ⚠ not all connected" if res.get("established") != n else "")])
    table("Idle connections (memory of all the server's containers)", ["Server", "Connections", "Established", "Before", "Peak", "Per connection"], rows)

    # fanout
    rows = []
    for n, size in data["settings"]["fanout"]:
        for s in servers:
            block = next((b for b in data["servers"][s].get("fanout", []) if b["connections"] == n), None)
            if not block:
                continue
            runs = block["runs"]
            ratio = med(r.get("delivered_ratio") for r in runs)
            rows.append([NAMES[s], str(n), fmt(med(r.get("last_p50_us") for r in runs), 1, 1000)
                         + spread([r.get("last_p50_us") for r in runs], 1, 1000),
                         fmt(med(r.get("last_p99_us") for r in runs), 1, 1000),
                         fmt(None if ratio is None else ratio * 100, 2, 1, " %") + flag(runs, bad=lambda r: "error" in r or r.get("delivered_ratio", 0) < 0.999)])
    table("Fan-out (one broadcast to every subscriber, 128 bytes)", ["Server", "Subscribers", "To the last (p50, ms)", "To the last (p99, ms)", "Delivered"], rows)

    print("\n#### What was run\n")
    for s in servers:
        print(f"- {NAMES[s]}: {data['servers'][s].get('versions', 'unknown')}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
