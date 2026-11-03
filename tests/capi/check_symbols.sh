#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# ABI guard (ADR-0016 section 8b): the dynamic symbols libwsrv.so defines must equal the checked-in
# include/wsrv/wsrv.symbols. An extra export (a leaked C++ symbol) and a missing one (a removed
# function: an ABI break) both fail. Usage: check_symbols.sh <libwsrv.so> <wsrv.symbols>
set -euo pipefail
lib=$1
expected=$2
# __bss_start, _edata, _end (and _init/_fini) are linker boilerplate that some builds export.
actual=$(nm -D --defined-only "$lib" | awk '$2 ~ /^[TDBRVWA]$/ {print $3}' \
    | grep -vE '^(__bss_start|_edata|_end|_init|_fini)$' | sort)
want=$(sort "$expected")
if [ "$actual" != "$want" ]; then
    echo "exported symbols differ from $expected (< expected, > actual):" >&2
    diff <(echo "$want") <(echo "$actual") >&2 || true
    exit 1
fi
