# SPDX-License-Identifier: Apache-2.0
"""Release builds must compile EVERY translation unit with the hardening flags (CODING_STANDARDS section 1).

Reads compile_commands.json. A single object built without -fcf-protection silently removes CET from the
whole linked binary, so third-party objects (GoogleTest) are checked too. Excluded: header-check TUs and
the never-built module-graph fixture, which do not end up in shipped binaries.
Usage: check_release_flags.py <compile_commands.json>
"""
import json
import platform
import sys

EXCLUDE = ("/header_check/", "/module_graph_fixture/")
# The last two switch on the standard library's own bounds checks (std::span, vector, string_view...):
# an out-of-range index aborts instead of being silent undefined behaviour. libstdc++ reads the first,
# libc++ the second; each ignores the other.
COMMON = [
    "-fstack-protector-strong",
    "-fstack-clash-protection",
    "-D_FORTIFY_SOURCE=3",
    "-D_GLIBCXX_ASSERTIONS",
    "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST",
]
ARCH = {"x86_64": ["-fcf-protection"], "aarch64": ["-mbranch-protection=standard"]}


def main() -> int:
    required = COMMON + ARCH.get(platform.machine(), [])
    entries = json.load(open(sys.argv[1], encoding="utf-8"))
    checked = 0
    failures = []
    for entry in entries:
        if any(part in entry["file"] for part in EXCLUDE):
            continue
        command = entry.get("command") or " ".join(entry["arguments"])
        missing = [flag for flag in required if flag not in command.split()]
        checked += 1
        if missing:
            failures.append(f"{entry['file']}: missing {' '.join(missing)}")
    if checked == 0:
        print("error: no translation units checked (empty compile_commands.json?)")
        return 2
    for line in failures:
        print(line)
    print(f"{checked} translation units checked, {len(failures)} without full hardening")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
