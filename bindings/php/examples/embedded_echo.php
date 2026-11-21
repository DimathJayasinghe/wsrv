<?php
// SPDX-License-Identifier: Apache-2.0
//
// An echo server that runs INSIDE this PHP process (embedded mode, ADR-0025): libwsrv.so is loaded
// through FFI, its own threads do the sockets, and the handlers below run on this thread.
//
//   1. Build or install libwsrv.so, then:  export WSRV_LIBRARY=/path/to/libwsrv.so
//   2. Run:  php examples/embedded_echo.php --port 8080
//   3. Connect a WebSocket client to ws://127.0.0.1:8080/. A command-line client sends no Origin
//      header: add --allow-missing-origin for local experiments only.
//
// Options: --host H  --port N  --allow-missing-origin  --tls-cert F --tls-key F  --deflate  --autobahn
// `--autobahn` raises size and rate limits for the conformance suite (tests/autobahn/run.sh --php);
// it is not a recommendation: these are the limits the suite needs, none is a library default.
//
// PHP-FPM cannot run this (the constructor refuses): use the sidecar, examples/echo.php.

declare(strict_types=1);

use Wsrv\ConnectionClosed;
use Wsrv\Embedded\Connection;
use Wsrv\Embedded\Server;

require __DIR__ . '/../tests/bootstrap.php';  // or vendor/autoload.php when installed with Composer

$options = getopt('', ['host:', 'port:', 'allow-missing-origin', 'tls-cert:', 'tls-key:', 'deflate', 'autobahn']);
if ($options === false) {
    fwrite(STDERR, "usage: embedded_echo.php [--host H] [--port N] [--allow-missing-origin] [--tls-cert F --tls-key F] [--deflate] [--autobahn]\n");
    exit(2);
}

$config = [
    'listen_host' => $options['host'] ?? '127.0.0.1',
    'listen_port' => (int) ($options['port'] ?? 8080),
    'allow_origin' => ['http://localhost:8000'],
    'allow_missing_origin' => isset($options['allow-missing-origin']),
    'permessage_deflate' => isset($options['deflate']),
];
if (isset($options['tls-cert'], $options['tls-key'])) {
    $config['tls_cert'] = $options['tls-cert'];
    $config['tls_key'] = $options['tls-key'];
}
if (isset($options['autobahn'])) {
    $config += [
        'max_message_size' => 20 * 1024 * 1024,   // cases 9.*: one message of up to 16 MiB
        'max_frame_payload' => 20 * 1024 * 1024,
        'max_fragments' => 1 << 20,                // cases 9.3.*, 9.4.*: 65 536 fragments
        'max_data_frames_per_second' => 2_000_000, // cases 9.7.*, 9.8.*, 10.1.1: bursts of tiny frames
        'event_queue_bytes' => 64 * 1024 * 1024,   // a 16 MiB message must fit the queue whole
        'max_connections_per_ip' => 0,             // the suite opens many connections from one address
        'max_new_connections_per_ip_per_second' => 0,
    ];
}

/**
 * Send back what came in. A full queue (false) is retried for about a second, then the client is
 * closed with 1013: this thread is the only sender, so retrying the SAME message keeps the order.
 */
function echo_back(Connection $c, string $data, bool $binary): void
{
    try {
        for ($attempt = 0; $attempt < 1000; $attempt++) {
            if ($binary ? $c->sendBinary($data) : $c->send($data)) {
                return;
            }
            usleep(1000);
        }
        $c->close(1013, 'slow reader');
    } catch (ConnectionClosed) {
        // Normal, not an error: the library failed or closed this connection (a protocol violation
        // right behind this message, or the peer left) before the echo was sent. The close event
        // follows. Without this catch the exception would leave run() and end the whole server.
    }
}

$server = new Server($config);
$server
    ->on('open', fn (Connection $c, string $peer) => fwrite(STDERR, "open {$c->id} from $peer\n"))
    ->on('message', echo_back(...))
    ->on('close', fn (Connection $c, int $code, string $reason, bool $clean) => fwrite(STDERR, "close {$c->id} code=$code clean=" . ($clean ? 'yes' : 'no') . "\n"));

pcntl_async_signals(true);
pcntl_signal(SIGTERM, static fn () => $server->stop());
pcntl_signal(SIGINT, static fn () => $server->stop());

$server->start();
fwrite(STDERR, 'embedded_echo: listening on port ' . $server->port() . " (Ctrl-C to stop)\n");
$server->run();
$server->close();
