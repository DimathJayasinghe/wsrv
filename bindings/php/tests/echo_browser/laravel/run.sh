#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# The whole product path with the real browser libraries (ADR-0042): a real Laravel application
# (session, CSRF, Broadcast::routes, the `wsrv` driver, `php artisan wsrv:echo`), a real wsrvd, and
# the real pusher-js and laravel-echo in your browser. Manual: it needs a Laravel 11+ application
# on disk (a skeleton is enough; it is COPIED, never modified), the two npm bundles and a browser.
#
#   LARAVEL_APP=/path/to/app WSRVD=/path/to/wsrvd \
#   PUSHER_JS=.../pusher-with-encryption.min.js ECHO_JS=.../echo.iife.js \
#   bash tests/echo_browser/laravel/run.sh
#
# It prints the page address (default http://127.0.0.1:8000/); open it, and the page runs the
# scenario and this script prints the result. Settings: APP_PORT (8000), WS_PORT (9001),
# BIND (127.0.0.1), PUBLIC_WS_PORT (= WS_PORT, for a published container port), PUBLIC_ORIGIN
# (http://127.0.0.1:APP_PORT, the page's origin as the browser sees it), TIMEOUT (240 seconds).
set -euo pipefail

: "${LARAVEL_APP:?set LARAVEL_APP to a Laravel 11+ application}" "${WSRVD:?set WSRVD}" "${PUSHER_JS:?set PUSHER_JS}" "${ECHO_JS:?set ECHO_JS}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../../.." && pwd)"          # bindings/php
APP_PORT="${APP_PORT:-8000}"; WS_PORT="${WS_PORT:-9001}"; BIND="${BIND:-127.0.0.1}"
PUBLIC_WS_PORT="${PUBLIC_WS_PORT:-$WS_PORT}"
PUBLIC_ORIGIN="${PUBLIC_ORIGIN:-http://127.0.0.1:$APP_PORT}"
TIMEOUT="${TIMEOUT:-240}"

WORK="$(mktemp -d)"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

cp -r "$LARAVEL_APP" "$WORK/app"
rm -f "$WORK/app/storage/logs/"*.log 2>/dev/null || true
cp -r "$HERE/overlay/." "$WORK/app/"
mkdir -p "$WORK/app/echo-test"; cp "$HERE/../index.html" "$WORK/app/echo-test/index.html"
mkdir -p "$WORK/app/public/vendor"; cp "$PUSHER_JS" "$WORK/app/public/vendor/pusher.js"; cp "$ECHO_JS" "$WORK/app/public/vendor/echo.js"
# The page loads its scripts from /vendor/..., which `php artisan serve` serves from public/.

"$WSRVD" --generate-token="$WORK/publish.token" >/dev/null
start_wsrvd() {
  "$WSRVD" --ipc-socket="$WORK/ipc.sock" --listen-host="$BIND" --listen-port="$WS_PORT" --allow-origin="$PUBLIC_ORIGIN" \
    --broker=true --publish-port=9200 --publish-token-file="$WORK/publish.token" --max-message-size=16384 \
    --shutdown-grace-ms=1500 >>"$WORK/wsrvd.log" 2>&1 &
  WSRVD_PID=$!
}
start_wsrvd; PIDS+=("$WSRVD_PID")

# `php artisan serve` passes only a few variables on to its server, so the settings go into the
# copy's .env (keeping its APP_KEY), which is what a real application does too.
sed -i "s#__PKG_SRC__#$PKG/src#" "$WORK/app/bootstrap/app.php"
{
  grep '^APP_KEY=' "$WORK/app/.env" || echo "APP_KEY=base64:$(head -c 32 /dev/urandom | base64)"
  cat <<ENV
APP_ENV=local
APP_DEBUG=true
SESSION_DRIVER=file
CACHE_STORE=file
QUEUE_CONNECTION=sync
LOG_CHANNEL=single
BROADCAST_CONNECTION=wsrv
WSRV_APP_KEY=laravel-echo-key
WSRV_APP_SECRET=laravel-echo-secret-$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')
WSRV_PUBLISH_URL=http://127.0.0.1:9200
WSRV_PUBLISH_TOKEN_FILE=$WORK/publish.token
WSRV_IPC=$WORK/ipc.sock
WSRV_ENCRYPTION_KEY=$(head -c 32 /dev/urandom | base64)
PUBLIC_WS_PORT=$PUBLIC_WS_PORT
WSRV_RESULT_FILE=$WORK/result.json
WSRV_RESTART_FLAG=$WORK/restart.flag
WSRV_RESTARTED_FLAG=$WORK/restarted.flag
ENV
} > "$WORK/app/.env.new"
mv "$WORK/app/.env.new" "$WORK/app/.env"

cd "$WORK/app"
sleep 1
php artisan wsrv:echo >"$WORK/echo.log" 2>&1 & PIDS+=("$!")
PHP_CLI_SERVER_WORKERS=4 php artisan serve --host="$BIND" --port="$APP_PORT" >"$WORK/serve.log" 2>&1 & PIDS+=("$!")
sleep 2
echo "open this page in a browser: $PUBLIC_ORIGIN/"

end=$((SECONDS + TIMEOUT))
while [ ! -f "$WORK/result.json" ] && [ $SECONDS -lt $end ]; do
  if [ -f "$WORK/restart.flag" ]; then
    # A deploy: stop the daemon (it closes every browser) and start it again. The worker, wsrv:echo,
    # must find it by itself, and the browsers must reconnect and resubscribe by themselves.
    rm -f "$WORK/restart.flag"
    kill "$WSRVD_PID"; wait "$WSRVD_PID" 2>/dev/null || true
    start_wsrvd; PIDS+=("$WSRVD_PID")
    touch "$WORK/restarted.flag"
  fi
  sleep 0.2
done
if [ ! -f "$WORK/result.json" ]; then echo "the page did not report in time"; echo "--- wsrv:echo"; tail -20 "$WORK/echo.log"; exit 1; fi

php -r '
$r = json_decode(file_get_contents($argv[1]), true); $failed = 0;
foreach ($r as $c) { echo ($c["ok"] ? "ok   " : "FAIL ") . $c["name"] . ($c["ok"] ? "" : ": " . $c["error"]) . "\n"; $failed += $c["ok"] ? 0 : 1; }
echo $failed === 0 ? "all browser checks passed (real Laravel)\n" : "$failed browser check(s) failed (real Laravel)\n";
exit($failed === 0 ? 0 : 1);' "$WORK/result.json"
status=$?
echo "--- wsrv:echo log"; cat "$WORK/echo.log" | tail -15
exit $status
