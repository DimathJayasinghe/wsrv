#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Usage: tools/format.sh check|fix     (check = fail on any difference; fix = rewrite in place)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-check}
cd "$root"
files=$(find src include tests examples -type f \( -name '*.cpp' -o -name '*.hpp' \) \
    -not -path '*/fixtures/*' 2>/dev/null | sort)
[ -z "$files" ] && exit 0
case "$mode" in
    check) echo "$files" | xargs clang-format --dry-run -Werror ;;
    fix)   echo "$files" | xargs clang-format -i ;;
    *) echo "usage: $0 check|fix" >&2; exit 2 ;;
esac
