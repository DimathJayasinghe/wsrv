#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enforce the module dependency graph on #include lines (docs/ARCHITECTURE.md section 2.1).

Two rules:
  1. A module may include only itself and its allowed dependencies ("protocol" must never include "net").
  2. The sans-I/O layers (base, crypto, http, protocol) must not include OS / network headers (ADR-0002).

Project headers are included as "<module>/<file>.hpp" (include root: src/). Exit codes: 0 clean,
1 violations (printed as file:line: message), 2 usage error.
"""
import argparse
import pathlib
import re
import sys

ALLOWED = {
    "base": set(),
    "crypto": {"base"},
    "http": {"base"},
    "protocol": {"base", "crypto", "http"},
    "extensions": {"base", "protocol"},
    "net": {"base"},
    "server": {"base", "protocol", "net", "extensions"},
}
SANS_IO = {"base", "crypto", "http", "protocol"}
OS_HEADER = re.compile(
    r"^(sys/|netinet/|arpa/|netdb\.h$|unistd\.h$|poll\.h$|fcntl\.h$|signal\.h$|epoll|openssl/)")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
SUFFIXES = {".hpp", ".h", ".cpp", ".cc"}


def violations(root: pathlib.Path):
    src = root / "src"
    for path in sorted(src.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        module = path.relative_to(src).parts[0]
        if module not in ALLOWED:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = INCLUDE.match(line)
            if not match:
                continue
            kind, header = match.groups()
            where = f"{path.relative_to(root)}:{number}"
            if kind == '"':
                other = header.split("/", 1)[0] if "/" in header else None
                if other in ALLOWED and other != module and other not in ALLOWED[module]:
                    yield f"{where}: {module} must not include {other}"
            elif module in SANS_IO and OS_HEADER.match(header):
                yield f"{where}: {module} must not include <{header}> (sans-I/O layer, ADR-0002)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    if not (args.root / "src").is_dir():
        print(f"error: {args.root}/src does not exist", file=sys.stderr)
        return 2
    found = list(violations(args.root))
    for message in found:
        print(message)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
