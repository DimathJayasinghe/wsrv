#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Fail if banned C functions are *called* in first-party code (docs/CODING_STANDARDS.md section 7).
# Usage: check_banned_functions.sh [path...]   (default: src/ and include/; missing paths are ignored)
# Matches `name(` so the word in prose ("operating system") or inside identifiers (system_clock) is fine.
# Output: file:line: banned function 'name'.
# Exit: 0 clean, 1 banned call found, 2 the scan itself failed (fail closed: an unreadable file is not "clean").
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
if [ "$#" -eq 0 ]; then set -- "$root/src" "$root/include"; fi

paths=()
for p in "$@"; do [ -e "$p" ] && paths+=("$p"); done
[ "${#paths[@]}" -eq 0 ] && exit 0

banned='strcpy|strcat|sprintf|sscanf|gets|strtok|atoi|atol|rand|srand|system|popen'
errfile=$(mktemp)
trap 'rm -f "$errfile"' EXIT

raw=$(grep -rnoHE --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.cc' \
    --include='*.cxx' --include='*.hh' --include='*.ipp' --include='*.inl' \
    "(^|[^A-Za-z0-9_])(${banned})[[:space:]]*\(" "${paths[@]}" 2>"$errfile")
rc=$?
if [ "$rc" -gt 1 ]; then
    echo "error: scan failed (grep exit $rc): $(cat "$errfile")" >&2
    exit 2
fi

# grep -o prints "file:line:<one non-word char><name>(": strip that down to the name.
hits=$(printf '%s\n' "$raw" \
    | sed -E "s/^([^:]+:[0-9]+):[^A-Za-z0-9_]?(${banned})[[:space:]]*\($/\1: banned function '\2'/" \
    | sed '/^$/d')
if [ -n "$hits" ]; then
    echo "$hits"
    exit 1
fi
exit 0
