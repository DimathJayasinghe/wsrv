#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the Autobahn|Testsuite against the echo example (docs/TESTING_STRATEGY.md section 6).
#
#   tests/autobahn/run.sh            full suite against the optimized build (the gate)
#   tests/autobahn/run.sh --quick    sections 1-7 and 10 (what a PR needs)
#   tests/autobahn/run.sh --wss      the same cases over TLS (wss://), with a throwaway certificate
#   tests/autobahn/run.sh --capi     the full suite against examples/echo_c, the pure-C server built
#                                    on wsrv.h only (M11 exit criterion); combine with --wss
#   tests/autobahn/run.sh --php      the full suite against bindings/php/examples/embedded_echo.php, the
#                                    PHP binding over FFI (M12 exit criterion); combine with --wss
#
# Host-side script: it drives `docker compose`. The server runs in the `dev` container, the suite in
# its own container on the same compose network. The optimized build is used because the full run
# sends 16 MiB messages and tens of thousands of frames; sanitizers make that time out.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

config=fuzzingclient.json
wss=0
capi=0
php=0
for arg in "$@"; do
    case "$arg" in
        --quick) config=fuzzingclient-quick.json ;;
        --wss) wss=1 ;;
        --capi) capi=1 ;;
        --php) php=1 ;;
        *) echo "usage: $0 [--quick | --wss | --capi [--wss] | --php [--wss]]" >&2; exit 2 ;;
    esac
done
if [[ "$wss" == 1 ]]; then config=fuzzingclient-wss.json; fi
if [[ "$capi" == 1 ]]; then
    # Its own agent name (wsrv-c, wsrv-c-wss) so the reports never mix with the C++ server's.
    if [[ "$config" == fuzzingclient-quick.json ]]; then
        echo "--capi runs the full suite; --quick does not apply" >&2; exit 2
    fi
    if [[ "$wss" == 1 ]]; then config=fuzzingclient-capi-wss.json; else config=fuzzingclient-capi.json; fi
fi
if [[ "$capi" == 1 && "$php" == 1 ]]; then echo "--capi and --php are different servers" >&2; exit 2; fi
if [[ "$php" == 1 ]]; then
    # Agent names wsrv-php and wsrv-php-wss, like wsrv-c for the C consumer.
    if [[ "$config" == fuzzingclient-quick.json ]]; then
        echo "--php runs the full suite; --quick does not apply" >&2; exit 2
    fi
    if [[ "$wss" == 1 ]]; then config=fuzzingclient-php-wss.json; else config=fuzzingclient-php.json; fi
fi
server=echo
if [[ "$capi" == 1 ]]; then server=echo_c; fi
if [[ "$php" == 1 ]]; then server=wsrv_shared; fi
report_dir=build/autobahn-reports
mkdir -p "$report_dir"
rm -f "$report_dir"/*.html "$report_dir"/*.json

docker compose up -d dev
docker compose exec -T dev bash -c "cmake --preset release -DWSRV_ENABLE_TLS=ON -DWSRV_ENABLE_DEFLATE=ON >/dev/null && cmake --build --preset release -j --target $server"

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
server_args="--host 0.0.0.0 --port 9001 --autobahn --deflate --allow-missing-origin $tls_args"
if [[ "$php" == 1 ]]; then
    # PHP: the same flags as the C++ and C servers; the library is the release build's libwsrv.so.
    server_cmd="WSRV_LIBRARY=\$PWD/build/release/src/capi/libwsrv.so php bindings/php/examples/embedded_echo.php $server_args"
    kill_pattern="bindings/php/examples/embedded_echo.php"
else
    server_cmd="./build/release/examples/$server $server_args"
    kill_pattern="build/release/examples/$server"
fi
docker compose exec -T -d dev bash -c "$server_cmd"
trap 'docker compose exec -T dev pkill -INT -f "$kill_pattern" || true' EXIT
sleep 1

AUTOBAHN_CONFIG="$config" docker compose --profile conformance run --rm autobahn
python3 tests/autobahn/summarize.py "$report_dir/index.json"
