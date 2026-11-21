<?php
// SPDX-License-Identifier: Apache-2.0
//
// An echo server that runs INSIDE this PHP process (embedded mode, ADR-0025): libwsrv.so is loaded
// through FFI, its own threads do the sockets, and the handlers below run on this thread.
//
//   1. Build or install libwsrv.so, then:  export WSRV_LIBRARY=/path/to/libwsrv.so
//   2. Run:  php examples/embedded_echo.php 8080
//   3. Connect a WebSocket client to ws://127.0.0.1:8080/. A command-line client sends no Origin
//      header; this example allows that for local experiments only (`allow_missing_origin`).
//
// PHP-FPM cannot run this (the constructor refuses): use the sidecar, examples/echo.php.

declare(strict_types=1);

use Wsrv\Embedded\Connection;
use Wsrv\Embedded\Server;

require __DIR__ . '/../tests/bootstrap.php';  // or vendor/autoload.php when installed with Composer

$server = new Server([
    'listen_port' => (int) ($argv[1] ?? 8080),
    'allow_origin' => ['http://localhost:8000'],
    'allow_missing_origin' => true,
]);
$server
    ->on('open', fn (Connection $c, string $peer) => fwrite(STDERR, "open {$c->id} from $peer\n"))
    ->on('message', function (Connection $c, string $data, bool $binary) {
        // false means "a queue is full": this example drops the slow client instead of buffering.
        $sent = $binary ? $c->sendBinary($data) : $c->send($data);
        if (!$sent) {
            $c->close(1013, 'slow reader');
        }
    })
    ->on('close', fn (Connection $c, int $code, string $reason, bool $clean) => fwrite(STDERR, "close {$c->id} code=$code clean=" . ($clean ? 'yes' : 'no') . "\n"));

pcntl_async_signals(true);
pcntl_signal(SIGTERM, static fn () => $server->stop());
pcntl_signal(SIGINT, static fn () => $server->stop());

$server->start();
fwrite(STDERR, 'listening on port ' . $server->port() . " (Ctrl-C to stop)\n");
$server->run();
$server->close();
