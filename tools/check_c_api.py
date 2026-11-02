#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that the public C header and the export list agree (ADR-0016 section 8).

include/wsrv/wsrv.h declares every exported function with WSRV_API; include/wsrv/wsrv.symbols lists the
names the shared library may export (the linker version script is generated from it). If they differ, the
header promises a function the library hides, or the library exports something the header never declared.
Exit codes: 0 equal, 1 differ (printed), 2 usage error.
"""
import argparse
import pathlib
import re
import sys


def declared(header_text: str) -> set:
    without_comments = re.sub(r"/\*.*?\*/", "", header_text, flags=re.S)
    return set(re.findall(r"WSRV_API\s+[^;(]*?\b(wsrv_\w+)\s*\(", without_comments))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    header = args.root / "include/wsrv/wsrv.h"
    symbols = args.root / "include/wsrv/wsrv.symbols"
    if not header.is_file() or not symbols.is_file():
        print("check_c_api: header or symbols file missing", file=sys.stderr)
        return 2
    in_header = declared(header.read_text(encoding="utf-8"))
    listed = {line.strip() for line in symbols.read_text(encoding="utf-8").splitlines() if line.strip()}
    for name in sorted(in_header - listed):
        print(f"wsrv.h declares {name} but wsrv.symbols does not list it")
    for name in sorted(listed - in_header):
        print(f"wsrv.symbols lists {name} but wsrv.h does not declare it with WSRV_API")
    return 0 if in_header == listed else 1


if __name__ == "__main__":
    sys.exit(main())
