#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Tests for tools/check_banned_functions.sh. Usage: test_banned_functions.sh <repo-root>
set -u
ROOT=${1:?usage: test_banned_functions.sh <repo-root>}
SCRIPT="$ROOT/tools/check_banned_functions.sh"
FIX="$ROOT/tests/tools/fixtures/banned"
fail=0
check() { if ! "$@"; then fail=1; fi; }

out=$("$SCRIPT" "$FIX/bad_strcpy.cpp" 2>&1); rc=$?
[ "$rc" -eq 1 ] || { echo "FAIL: bad_strcpy.cpp should exit 1, got $rc"; fail=1; }
echo "$out" | grep -q "bad_strcpy.cpp:5: banned function 'strcpy'" \
    || { echo "FAIL: missing location/name in: $out"; fail=1; }

out=$("$SCRIPT" "$FIX/good.hpp" 2>&1); rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: good.hpp should exit 0, got $rc: $out"; fail=1; }

# A non-existent default path (include/) must not be an error.
out=$("$SCRIPT" "$FIX/good.hpp" "$FIX/no_such_dir" 2>&1); rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: missing path should be ignored, got $rc: $out"; fail=1; }

[ "$fail" -eq 0 ] && echo "banned-function tests: OK"
exit "$fail"
