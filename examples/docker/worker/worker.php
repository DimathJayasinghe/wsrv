<?php
// Echo worker: the daemon owns the WebSockets, this script only reacts to events.
require __DIR__ . '/vendor/autoload.php';

use Wsrv\{Client, Connection, Worker};

$worker = new Worker(Client::connect('/run/wsrv/ipc.sock'));
$worker->on('message', fn (Connection $c, string $data, bool $binary) =>
    $binary ? $c->sendBinary($data) : $c->send($data));
$worker->run();
