<?php
// A PHP worker that runs on YOUR machine (macOS, Windows or Linux) and reaches wsrvd over TCP (wsrv 1.1, ADR-0038).
//   WSRV_IPC     where the daemon's IPC port is   (default tcp://127.0.0.1:9300)
//   WSRV_TOKEN   the shared secret the daemon's `wsrvd --generate-token` made (see compose.yaml for how to read it)
// Echoes every message; "join <room>" puts the connection in a room (needs WSRVD_BROKER=true on the daemon).
require __DIR__ . '/vendor/autoload.php';

use Wsrv\{Connection, Supervisor, Worker, WsrvException};

if (!method_exists(Wsrv\Client::class, 'isTcpEndpoint')) {
    // wsrv/client 1.0 would read "tcp://..." as a socket path and fail with a confusing message.
    fwrite(STDERR, "this needs wsrv/client 1.1 or newer for TCP: composer require \"wsrv/client:^1.1\"\n");
    exit(2);
}

$endpoint = getenv('WSRV_IPC') ?: 'tcp://127.0.0.1:9300';
$token = getenv('WSRV_TOKEN') ?: '';
if ($token === '') {
    fwrite(STDERR, "WSRV_TOKEN is missing: see examples/host-worker/compose.yaml for how to read it\n");
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
