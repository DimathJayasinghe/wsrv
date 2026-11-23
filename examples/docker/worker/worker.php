<?php
// The whole PHP side: the daemon owns the WebSockets, this script only reacts to events.
//   "join <room>"   puts the connection in a room (the worker decides; a browser never names a room to the daemon)
//   anything else   is echoed back
// Messages to a room come from publish.php (or any process that can POST to the daemon: see the README).
require __DIR__ . '/vendor/autoload.php';

use Wsrv\{Client, Connection, Worker, WsrvException};

$worker = new Worker(Client::connect('/run/wsrv/ipc.sock'));
$worker->on('message', function (Connection $c, string $data, bool $binary): void {
    if ($binary) {
        $c->sendBinary($data);
    } elseif (str_starts_with($data, 'join ')) {
        try {
            $c->subscribe(substr($data, 5));     // a bad name (spaces, "!") is refused here, before anything is sent
            $c->send('joined ' . substr($data, 5));
        } catch (WsrvException) {
            $c->send('error: a room is letters, digits and . _ : / @ -');
        }
    } else {
        $c->send($data);
    }
});
$worker->run();
