#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the Autobahn|Testsuite against the echo example (docs/TESTING_STRATEGY.md section 6).
#
#   tests/autobahn/run.sh            full suite against the optimized build (the gate)
#   tests/autobahn/run.sh --quick    sections 1-7 and 10 (what a PR needs)
#
# Host-side script: it drives `docker compose`. The server runs in the `dev` container, the suite in
# its own container on the same compose network. The optimized build is used because the full run
# sends 16 MiB messages and tens of thousands of frames; sanitizers make that time out.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

config=fuzzingclient.json
if [[ "${1:-}" == "--quick" ]]; then
    config=fuzzingclient-quick.json
fi
report_dir=build/autobahn-reports
mkdir -p "$report_dir"
rm -f "$report_dir"/*.html "$report_dir"/*.json

docker compose up -d dev
docker compose exec -T dev bash -c 'cmake --preset release >/dev/null && cmake --build --preset release -j --target echo'

# --autobahn raises size/rate limits only (see examples/echo/main.cpp); --allow-missing-origin
# because the suite is not a browser. Neither is a library default.
docker compose exec -T -d dev bash -c \
    './build/release/examples/echo --host 0.0.0.0 --port 9001 --autobahn --allow-missing-origin'
trap 'docker compose exec -T dev pkill -INT -f build/release/examples/echo || true' EXIT
sleep 1

AUTOBAHN_CONFIG="$config" docker compose --profile conformance run --rm autobahn
python3 tests/autobahn/summarize.py "$report_dir/index.json"
