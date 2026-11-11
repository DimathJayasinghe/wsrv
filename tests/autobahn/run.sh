#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the Autobahn|Testsuite against the echo example (docs/TESTING_STRATEGY.md section 6).
#
#   tests/autobahn/run.sh            full suite against the optimized build (the gate)
#   tests/autobahn/run.sh --quick    sections 1-7 and 10 (what a PR needs)
#   tests/autobahn/run.sh --wss      the same cases over TLS (wss://), with a throwaway certificate
#
# Host-side script: it drives `docker compose`. The server runs in the `dev` container, the suite in
# its own container on the same compose network. The optimized build is used because the full run
# sends 16 MiB messages and tens of thousands of frames; sanitizers make that time out.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

config=fuzzingclient.json
wss=0
case "${1:-}" in
    --quick) config=fuzzingclient-quick.json ;;
    --wss) config=fuzzingclient-wss.json; wss=1 ;;
    "") ;;
    *) echo "usage: $0 [--quick | --wss]" >&2; exit 2 ;;
esac
report_dir=build/autobahn-reports
mkdir -p "$report_dir"
rm -f "$report_dir"/*.html "$report_dir"/*.json

docker compose up -d dev
docker compose exec -T dev bash -c 'cmake --preset release -DWSRV_ENABLE_TLS=ON -DWSRV_ENABLE_DEFLATE=ON >/dev/null && cmake --build --preset release -j --target echo'

# --autobahn raises size/rate limits only (see examples/echo/main.cpp); --allow-missing-origin
# because the suite is not a browser; --deflate offers permessage-deflate (cases 12.* and 13.*; the
# other cases never ask for it). None of these is a library default.
tls_args=""
if [[ "$wss" == 1 ]]; then
    # A self-signed certificate: the suite's client does not verify it, and TLS behaviour (not trust)
    # is what this run checks. The grade of the TLS settings is tests/tls/run_testssl.sh.
    docker compose exec -T dev bash -c 'cd /tmp && rm -f wss.key wss.pem &&
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout wss.key \
            -out wss.pem -days 2 -subj "/CN=dev" >/dev/null 2>&1 && chmod 600 wss.key'
    tls_args="--tls-cert /tmp/wss.pem --tls-key /tmp/wss.key"
fi
docker compose exec -T -d dev bash -c \
    "./build/release/examples/echo --host 0.0.0.0 --port 9001 --autobahn --deflate --allow-missing-origin $tls_args"
trap 'docker compose exec -T dev pkill -INT -f build/release/examples/echo || true' EXIT
sleep 1

AUTOBAHN_CONFIG="$config" docker compose --profile conformance run --rm autobahn
python3 tests/autobahn/summarize.py "$report_dir/index.json"
