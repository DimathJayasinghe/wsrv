<?php
// SPDX-License-Identifier: Apache-2.0
//
// Chat rooms on the embedded server (see Chat.php for the protocol).
//
//   export WSRV_LIBRARY=/path/to/libwsrv.so
//   php examples/chat/main.php --port 9002 --allow-origin http://localhost:8000
//
// Options: --host H  --port N  --allow-origin URL  --allow-missing-origin
// `join lobby`, `say lobby hello`, `members lobby`, `rooms`, `leave lobby`: one text message each.

declare(strict_types=1);

use Wsrv\Embedded\Connection;
use Wsrv\Embedded\Server;
use Wsrv\Examples\Chat;

require __DIR__ . '/../../tests/bootstrap.php';  // or vendor/autoload.php when installed with Composer
require __DIR__ . '/Chat.php';

$options = getopt('', ['host:', 'port:', 'allow-origin:', 'allow-missing-origin']);
if ($options === false) {
    fwrite(STDERR, "usage: main.php [--host H] [--port N] [--allow-origin URL] [--allow-missing-origin]\n");
    exit(2);
}
$origins = (array) ($options['allow-origin'] ?? []);

$chat = new Chat();
$server = new Server([
    'listen_host' => $options['host'] ?? '127.0.0.1',
    'listen_port' => (int) ($options['port'] ?? 9002),
    'allow_origin' => array_values($origins),
    'allow_missing_origin' => isset($options['allow-missing-origin']),
]);
$server
    ->on('message', $chat->onMessage(...))
    ->on('close', fn (Connection $c, int $code, string $reason, bool $clean) => $chat->onClose($c));

pcntl_async_signals(true);
pcntl_signal(SIGTERM, static fn () => $server->stop());
pcntl_signal(SIGINT, static fn () => $server->stop());

$server->start();
fwrite(STDERR, 'chat: listening on port ' . $server->port() . " (Ctrl-C to stop)\n");
$server->run();
$server->close();
