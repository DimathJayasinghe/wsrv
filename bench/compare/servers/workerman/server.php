<?php
// The benchmark application on Workerman (single process, the event extension): same behaviour as every contender.
declare(strict_types=1);

require __DIR__ . '/vendor/autoload.php';

use Workerman\Connection\TcpConnection;
use Workerman\Worker;

$ws = new Worker('websocket://0.0.0.0:8080');
$ws->count = 1;
$ws->onMessage = function (TcpConnection $connection, string $data) use ($ws): void {
    if (str_starts_with($data, 'bcast:')) {
        $payload = substr($data, 6);
        foreach ($ws->connections as $peer) {
            $peer->send($payload);
        }
    } else {
        $connection->send($data);
    }
};
Worker::runAll();
