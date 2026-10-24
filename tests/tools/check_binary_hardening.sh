#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Assert a built binary is a PIE with full RELRO (CODING_STANDARDS section 1). Usage: <binary>
set -u
bin=${1:?usage: check_binary_hardening.sh <binary>}
fail=0
readelf -h "$bin" | grep -q 'Type:.*DYN' || { echo "FAIL: $bin is not a position-independent executable"; fail=1; }
readelf -lW "$bin" | grep -q GNU_RELRO || { echo "FAIL: $bin has no GNU_RELRO segment"; fail=1; }
readelf -dW "$bin" | grep -qE 'BIND_NOW|FLAGS.*NOW' || { echo "FAIL: $bin lacks BIND_NOW (partial RELRO only)"; fail=1; }
[ "$fail" -eq 0 ] && echo "binary hardening OK: PIE, RELRO, BIND_NOW"
exit "$fail"
