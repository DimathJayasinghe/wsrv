#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-module line and branch coverage of src/ against NFR-M4 (docs/REQUIREMENTS.md section 3.4).

Reads the gcovr JSON summary and exits 1 if a module is below its threshold:
protocol, http, base, crypto: 90 % lines and 85 % branches; every other module: 75 % lines.
"""
import collections
import json
import sys

STRICT = {"protocol", "http", "base", "crypto"}


def main(path: str) -> int:
    with open(path, encoding="utf-8") as handle:
        summary = json.load(handle)
    modules = collections.defaultdict(lambda: [0, 0, 0, 0])  # lines, covered, branches, covered
    for entry in summary["files"]:
        parts = entry["filename"].replace("../../", "").split("/")
        module = parts[1] if parts[0] == "src" else parts[0]
        row = modules[module]
        row[0] += entry["line_total"]
        row[1] += entry["line_covered"]
        row[2] += entry["branch_total"]
        row[3] += entry["branch_covered"]
    print(f"{'module':11} {'lines':>15} {'line%':>7} {'branches':>15} {'branch%':>8}  need")
    total = [0, 0, 0, 0]
    below = []
    for name in sorted(modules):
        lines, covered, branches, branches_covered = modules[name]
        line_pct = 100 * covered / lines if lines else 100.0
        branch_pct = 100 * branches_covered / branches if branches else 100.0
        strict = name in STRICT
        ok = (line_pct >= 90 and branch_pct >= 85) if strict else line_pct >= 75
        if not ok:
            below.append(name)
        print(f"{name:11} {covered:>7}/{lines:<7} {line_pct:6.1f}% {branches_covered:>7}/{branches:<7} "
              f"{branch_pct:7.1f}%  {'90/85' if strict else '75/-'}  {'OK' if ok else 'BELOW'}")
        for i, value in enumerate(modules[name]):
            total[i] += value
    print(f"TOTAL {100 * total[1] / total[0]:.1f}% lines ({total[1]}/{total[0]}), "
          f"{100 * total[3] / total[2]:.1f}% branches ({total[3]}/{total[2]})")
    if below:
        print("BELOW THE THRESHOLD:", ", ".join(below), file=sys.stderr)
    return 1 if below else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/coverage-src-summary.json"))
