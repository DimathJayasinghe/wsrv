<?php
// SPDX-License-Identifier: Apache-2.0
//
// An echo server for wsrvd, in the callback style.
//
//   1. Start the daemon:   wsrvd --ipc-socket=/tmp/wsrv/ipc.sock --listen-port=8080 \
//                                --allow-origin=http://localhost:8000
//      (the directory must be private: mode 0700, owned by you)
//   2. Run this script:    php examples/echo.php /tmp/wsrv/ipc.sock
//   3. Connect any WebSocket client to ws://127.0.0.1:8080/ and send something.

declare(strict_types=1);

use Wsrv\Client;
use Wsrv\Connection;
use Wsrv\Worker;

require __DIR__ . '/../tests/bootstrap.php';  // or vendor/autoload.php when installed with Composer

$socket = $argv[1] ?? '/tmp/wsrv/ipc.sock';
$token = getenv('WSRV_IPC_TOKEN') ?: '';   // never take a secret from the command line

$worker = new Worker(Client::connect($socket, $token));
$worker
    ->on('open', fn (Connection $c, string $peer) => fwrite(STDERR, "open {$c->id} from $peer\n"))
    ->on('message', fn (Connection $c, string $data, bool $binary) => $binary ? $c->sendBinary($data) : $c->send($data))
    ->on('close', fn (Connection $c, int $code, string $reason, bool $clean) => fwrite(STDERR, "close {$c->id} code=$code clean=" . ($clean ? 'yes' : 'no') . "\n"));

pcntl_async_signals(true);
pcntl_signal(SIGTERM, static fn () => $worker->stop());
pcntl_signal(SIGINT, static fn () => $worker->stop());

$worker->run();
