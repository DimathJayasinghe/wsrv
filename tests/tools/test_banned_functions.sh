#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Tests for tools/check_banned_functions.sh. Usage: test_banned_functions.sh <repo-root>
set -u
ROOT=${1:?usage: test_banned_functions.sh <repo-root>}
SCRIPT="$ROOT/tools/check_banned_functions.sh"
FIX="$ROOT/tests/tools/fixtures/banned"
fail=0

expect_hit() {  # file expected-line expected-name
    local out rc
    out=$("$SCRIPT" "$FIX/$1" 2>&1); rc=$?
    [ "$rc" -eq 1 ] || { echo "FAIL: $1 should exit 1, got $rc: $out"; fail=1; }
    echo "$out" | grep -q "$1:$2: banned function '$3'" \
        || { echo "FAIL: $1 expected line $2 name '$3', got: $out"; fail=1; }
}

expect_hit bad_strcpy.cpp 5 strcpy
expect_hit bad_sscanf.cpp 6 sscanf
expect_hit bad_srand.cpp 5 srand            # must not be reported as 'rand'
expect_hit bad_std_system.cpp 5 system

out=$("$SCRIPT" "$FIX/good.hpp" 2>&1); rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: good.hpp should exit 0, got $rc: $out"; fail=1; }

# A non-existent path (e.g. include/ before it exists) must not be an error.
out=$("$SCRIPT" "$FIX/good.hpp" "$FIX/no_such_dir" 2>&1); rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: missing path should be ignored, got $rc: $out"; fail=1; }

# Fail closed: if grep itself cannot read a file, the gate must fail, not report "clean".
if [ "$(id -u)" -ne 0 ]; then
    tmp=$(mktemp -d)
    printf 'int main() { return 0; }\n' > "$tmp/unreadable.cpp"
    chmod 000 "$tmp/unreadable.cpp"
    out=$("$SCRIPT" "$tmp" 2>&1); rc=$?
    chmod 600 "$tmp/unreadable.cpp"; rm -rf "$tmp"
    [ "$rc" -ne 0 ] || { echo "FAIL: unreadable file must make the gate fail, got rc=$rc: $out"; fail=1; }
fi

[ "$fail" -eq 0 ] && echo "banned-function tests: OK"
exit "$fail"
