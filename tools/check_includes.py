#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enforce the module dependency graph on #include lines (docs/ARCHITECTURE.md section 2.1).

Rules (both include forms, "..." and <...>, are checked):
  1. A module may include only itself and its allowed dependencies ("protocol" must never include "net").
     Relative includes ("../net/x.hpp") are resolved before checking.
  2. The sans-I/O layers (base, crypto, http, protocol) must not include OS / network / threading-runtime
     headers (ADR-0002).
  3. <openssl/...> may be included only by the tls module (ADR-0019): everything else sees TLS through
     interfaces.
  4. Every directory under src/ must be a known module, and project includes must name a known module.

Project headers are included as "<module>/<file>.hpp" (include root: src/). A header whose first path
component is a module name and whose suffix is not ".h" is a project header (so <net/if.h> is the OS one).
Exit codes: 0 clean, 1 violations (printed as file:line: message), 2 usage error.
"""
import argparse
import os
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
    # OpenSSL TLS transport (ADR-0019): the only module that may include <openssl/...>.
    "tls": {"base", "net"},
    "server": {"base", "http", "protocol", "net", "extensions"},
    "capi": {"base", "net", "server"},
    "ipc": {"base"},
    # The sidecar daemon (ADR-0017) lives in apps/wsrvd/, not src/, but obeys the same graph.
    "wsrvd": {"base", "net", "ipc", "capi"},
}
SANS_IO = {"base", "crypto", "http", "protocol", "ipc"}
OS_HEADER = re.compile(
    r"^(sys/|netinet/|arpa/|linux/|asm/|net/if\.h$|netdb\.h$|unistd\.h$|poll\.h$|fcntl\.h$|signal\.h$"
    r"|pthread\.h$|ifaddrs\.h$|dlfcn\.h$|sched\.h$|semaphore\.h$|termios\.h$|windows\.h$"
    r"|winsock2\.h$|ws2tcpip\.h$|mswsock\.h$|epoll|openssl/)")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
SUFFIXES = {".hpp", ".hh", ".hxx", ".h", ".ipp", ".inl", ".tpp", ".cpp", ".cc", ".cxx"}
PROJECT_HEADER_SUFFIXES = {".hpp", ".hh", ".hxx", ".ipp", ".inl", ".tpp"}


def project_module(header: str, including_file: pathlib.Path, src: pathlib.Path):
    """Return (module-or-None, is_project_header) for an include target."""
    suffix = pathlib.PurePosixPath(header).suffix
    if header.startswith(".") or "/../" in header:
        resolved = pathlib.Path(os.path.normpath(including_file.parent / header))
        try:
            parts = resolved.relative_to(src).parts
        except ValueError:
            return None, False  # escapes src/: not a project header
        return (parts[0] if parts else None), True
    first = header.split("/", 1)[0] if "/" in header else None
    if first in ALLOWED and suffix != ".h":
        return first, True
    if first is not None and suffix in PROJECT_HEADER_SUFFIXES and not OS_HEADER.match(header):
        return first, True  # looks like a project header of a module that does not exist
    return None, False


def file_violations(path: pathlib.Path, module: str, where_file: pathlib.Path, src: pathlib.Path):
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        match = INCLUDE.match(line)
        if not match:
            continue
        _kind, header = match.groups()
        where = f"{where_file}:{number}"
        other, is_project = project_module(header, path, src)
        if is_project:
            if other not in ALLOWED:
                yield f"{where}: {module} includes unknown module '{other}'"
            elif other != module and other not in ALLOWED[module]:
                yield f"{where}: {module} must not include {other}"
        elif module in SANS_IO and OS_HEADER.match(header):
            yield f"{where}: {module} must not include <{header}> (sans-I/O layer, ADR-0002)"
        elif module != "tls" and header.startswith("openssl/"):
            yield f"{where}: {module} must not include <{header}> (only tls may, ADR-0019)"


def violations(root: pathlib.Path):
    src = root / "src"
    for path in sorted(src.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        module = path.relative_to(src).parts[0]
        where_file = path.relative_to(root)
        if module not in ALLOWED:
            yield f"{where_file}:1: unknown module '{module}' (not in ARCHITECTURE section 2.1)"
            continue
        yield from file_violations(path, module, where_file, src)
    daemon = root / "apps" / "wsrvd"
    if daemon.is_dir():
        for path in sorted(daemon.rglob("*")):
            if path.is_file() and path.suffix in SUFFIXES:
                yield from file_violations(path, "wsrvd", path.relative_to(root), src)


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
