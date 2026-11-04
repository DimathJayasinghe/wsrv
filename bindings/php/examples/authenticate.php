<?php
// SPDX-License-Identifier: Apache-2.0
//
// Decide who may connect, from the session cookie the browser sends (ADR-0018).
//
//   1. Start the daemon with the hook on and the Cookie header forwarded:
//        wsrvd --ipc-socket=/tmp/wsrv/ipc.sock --listen-port=8080 --allow-origin=http://localhost:8000 \
//              --handshake-hook=true --handshake-forward-header=Cookie
//      (Cookie and Authorization are never forwarded unless you list them.)
//   2. Run this script:    php examples/authenticate.php /tmp/wsrv/ipc.sock
//   3. Open a WebSocket from a page that has a cookie `session=<a token from $sessions below>`.
//      Without it the browser is refused with 403; with an expired one, 401.
//
// The handler must be quick: the browser waits for it (the daemon refuses with 503 after
// handshake_decision_timeout_ms, default 3 s). Replace the array with a cache lookup.

declare(strict_types=1);

use Wsrv\Client;
use Wsrv\Connection;
use Wsrv\Upgrade;
use Wsrv\Worker;

require __DIR__ . '/../tests/bootstrap.php';  // or vendor/autoload.php when installed with Composer

$socket = $argv[1] ?? '/tmp/wsrv/ipc.sock';
$token = getenv('WSRV_IPC_TOKEN') ?: '';

/** session token => user name; "expired" sessions are known but no longer valid */
$sessions = ['s3cr3t-alice' => 'alice', 's3cr3t-bob' => 'bob'];
$expired = ['old-token' => true];
$users = [];  // connection id => user name, filled when the connection opens

$worker = new Worker(Client::connect($socket, $token));
$worker
    ->on('authenticate', function (Upgrade $u) use ($sessions, $expired, &$users): bool|int {
        $session = $u->request->cookie('session');
        if ($session === null) {
            return false;               // 403: not logged in
        }
        if (isset($expired[$session])) {
            return 401;                 // 401: the browser should log in again
        }
        if (!isset($sessions[$session])) {
            return false;
        }
        $users[$u->conn] = $sessions[$session];  // the id of the question is the id of the connection
        return true;
    })
    ->on('open', fn (Connection $c) => $c->send('welcome, ' . ($users[$c->id] ?? '?')))
    ->on('message', fn (Connection $c, string $data) => $c->send(($users[$c->id] ?? '?') . ': ' . $data))
    ->on('close', function (Connection $c) use (&$users): void {
        unset($users[$c->id]);
    });
$worker->run();
