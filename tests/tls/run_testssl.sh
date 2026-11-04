#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Grade the TLS configuration of a real wsrvd with testssl.sh (M8 exit criterion: grade A or better).
#
#   tests/tls/run_testssl.sh [TESTSSL_DIR]      (run inside the dev container: `make shell`)
#
# TESTSSL_DIR defaults to build/testssl. Get it once (it is a third-party bash script, so keep it out
# of the source tree and read it before running it):
#   git clone --depth 1 https://github.com/testssl/testssl.sh.git build/testssl
# It needs `hexdump` (package bsdextrautils) and `dig` is optional. The last grade recorded in
# ADR-0019 amendment 6 used testssl.sh commit 6f0afec.
#
# A throwaway CA signs the server certificate and is given to testssl.sh with --add-ca, so the
# certificate chain counts as trusted (a self-signed certificate would cap the grade at T and say
# nothing about our TLS settings). Nothing here touches the network except 127.0.0.1.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
testssl_dir="${1:-$root/build/testssl}"
wsrvd="${WSRVD:-$root/build/dev/apps/wsrvd/wsrvd}"
port="${PORT:-18445}"
[[ -x "$testssl_dir/testssl.sh" ]] || { echo "run_testssl: no $testssl_dir/testssl.sh (see the header)"; exit 2; }
[[ -x "$wsrvd" ]] || { echo "run_testssl: build wsrvd first ($wsrvd)"; exit 2; }

work="$(mktemp -d)"
pid=""
cleanup() {
    if [[ -n "$pid" ]]; then kill -TERM "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
cd "$work"

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca.key -out ca.pem \
    -days 2 -subj "/CN=wsrv test CA" -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign" >/dev/null 2>&1
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem -out leaf.csr \
    -subj "/CN=localhost" >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\n' >ext.cnf
openssl x509 -req -in leaf.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out chain.pem -days 2 \
    -extfile ext.cnf >/dev/null 2>&1
chmod 600 key.pem

"$wsrvd" --ipc-socket="$work/ipc.sock" --listen-port="$port" \
    --tls-cert="$work/chain.pem" --tls-key="$work/key.pem" 2>"$work/wsrvd.log" &
pid=$!
sleep 1

"$testssl_dir/testssl.sh" --add-ca "$work/ca.pem" --warnings off --color 0 --quiet \
    "localhost:$port" | tee "$work/report.txt"

grade="$(sed -n 's/^ *Overall Grade *//p' "$work/report.txt" | head -n 1)"
echo "run_testssl: overall grade '${grade:-none}'"
case "$grade" in
    A+|A|A-) exit 0 ;;
    *) echo "run_testssl: FAILED: grade A or better is required" >&2; exit 1 ;;
esac
