<?php
// A PHP worker that runs on YOUR machine (macOS, Windows or Linux) and reaches wsrvd over TCP (wsrv 1.1, ADR-0038).
//   WSRV_IPC     where the daemon's IPC port is   (default tcp://127.0.0.1:9300)
//   WSRV_TOKEN   the shared secret, at least 32 bytes (see compose.yaml for how to read it)
// Echoes every message; "join <room>" puts the connection in a room (needs WSRVD_BROKER=true on the daemon).
require __DIR__ . '/vendor/autoload.php';

use Wsrv\{Connection, Supervisor, Worker, WsrvException};

$endpoint = getenv('WSRV_IPC') ?: 'tcp://127.0.0.1:9300';
$token = getenv('WSRV_TOKEN') ?: '';
if (strlen($token) < 32) {
    fwrite(STDERR, "WSRV_TOKEN is missing or shorter than 32 bytes: see examples/host-worker/compose.yaml\n");
    exit(2);
}

// forSocket() takes the same endpoint forms as Client::connect(): a socket path, or tcp://host:port.
Supervisor::forSocket($endpoint, $token)->run(function (Worker $worker): void {
    $worker->on('message', function (Connection $c, string $data, bool $binary): void {
        if ($binary) {
            $c->sendBinary($data);
        } elseif (str_starts_with($data, 'join ')) {
            try {
                $c->subscribe(substr($data, 5));
                $c->send('joined ' . substr($data, 5));
            } catch (WsrvException) {
                $c->send('error: a room is letters, digits and . _ : / @ -');
            }
        } else {
            $c->send($data);
        }
    });
});
