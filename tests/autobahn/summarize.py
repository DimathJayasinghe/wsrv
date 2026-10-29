#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarize an Autobahn index.json and fail on any FAILED case.

Autobahn verdicts: OK, NON-STRICT (allowed but not the preferred behaviour), INFORMATIONAL (no
verdict defined), UNIMPLEMENTED, FAILED. Only FAILED fails the gate; NON-STRICT cases are listed so
each one can be justified in EXCLUDED.md.
"""
import collections
import json
import sys


def main(path: str) -> int:
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)
    counts = collections.Counter()
    failed, non_strict = [], []
    for agent, cases in report.items():
        for case, result in sorted(cases.items()):
            # behaviorClose is the verdict on the closing handshake; both must be acceptable.
            for verdict in (result["behavior"], result["behaviorClose"]):
                counts[verdict] += 1
                if verdict == "FAILED":
                    failed.append(f"{agent} {case}")
                elif verdict == "NON-STRICT":
                    non_strict.append(f"{agent} {case}")
    print("Autobahn verdicts:", dict(counts))
    for case in non_strict:
        print("  non-strict:", case)
    for case in failed:
        print("  FAILED:", case)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
