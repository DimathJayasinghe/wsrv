<?php
// The benchmark application on Swoole (one worker): same behaviour as every contender.
declare(strict_types=1);

use Swoole\WebSocket\Frame;
use Swoole\WebSocket\Server;

$server = new Server('0.0.0.0', 8080);
$server->set(['worker_num' => 1, 'max_connection' => 20000, 'log_level' => SWOOLE_LOG_WARNING]);
$server->on('message', function (Server $server, Frame $frame): void {
    if (str_starts_with($frame->data, 'bcast:')) {
        $payload = substr($frame->data, 6);
        foreach ($server->connections as $fd) {
            if ($server->isEstablished($fd)) {
                $server->push($fd, $payload);
            }
        }
    } else {
        $server->push($frame->fd, $frame->data);
    }
});
$server->start();
