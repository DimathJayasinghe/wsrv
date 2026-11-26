<?php
// SPDX-License-Identifier: Apache-2.0
// End to end over the TCP IPC transport (ADR-0038): the REAL wsrvd listening with --ipc-port, a
// Wsrv\Worker connected with tcp://, and a raw WebSocket client.
// Run with: php tests/tcp_interop.php /path/to/wsrvd

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

use Wsrv\Client;
use Wsrv\Connection;
use Wsrv\HandshakeRefused;
use Wsrv\StatusCode;
use Wsrv\Worker;

require __DIR__ . '/ws_client.php';

$daemonPath = $argv[1] ?? fail('usage: php tests/tcp_interop.php /path/to/wsrvd');
$dir = sys_get_temp_dir() . '/wsrv-php-tcp-' . bin2hex(random_bytes(4));
mkdir($dir, 0700) || fail('mkdir');
$tokenFile = $dir . '/ipc.token';
$token = bin2hex(random_bytes(24));  // 48 characters: over the daemon's 32-byte floor for TCP
file_put_contents($tokenFile, $token) !== false || fail('write the token');
chmod($tokenFile, 0600);

$process = proc_open(
    [$daemonPath, '--ipc-port=0', "--ipc-token-file=$tokenFile", '--listen-port=0',
     '--allow-origin=https://app.example.com', '--shutdown-grace-ms=1500'],
    [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['pipe', 'w']],
    $pipes
) ?: fail('cannot start wsrvd');

$wsPort = 0;
$endpoint = '';
$log = '';
$deadline = microtime(true) + 10;
while (($wsPort === 0 || $endpoint === '') && microtime(true) < $deadline) {
    $r = [$pipes[2]];
    $w = $e = null;
    if (stream_select($r, $w, $e, 0, 200000) > 0) {
        $line = fgets($pipes[2]);
        $log .= (string) $line;
        if ($line !== false && preg_match('/listening on port (\d+)/', $line, $m)) {
            $wsPort = (int) $m[1];
        }
        if ($line !== false && preg_match('/ipc tcp ([0-9.]+:\d+) ready/', $line, $m)) {
            $endpoint = 'tcp://' . $m[1];
        }
    }
}
($wsPort > 0 && $endpoint !== '') || fail("wsrvd did not start:\n$log");

$failures = [];
$check = function (string $name, callable $body) use (&$failures): void {
    try {
        $body();
        echo "ok   $name\n";
    } catch (Throwable $t) {
        $failures[] = $name;
        echo "FAIL $name: " . $t->getMessage() . "\n";
    }
};

$check('a wrong token is refused with INVALID_ARGUMENT', function () use ($endpoint) {
    $e = assertThrows(HandshakeRefused::class, fn () => Client::connect($endpoint, str_repeat('x', 48), 5.0));
    assertSame(StatusCode::INVALID_ARGUMENT, $e->status);
});

$check('no token at all is refused the same way (nothing about why)', function () use ($endpoint) {
    $e = assertThrows(HandshakeRefused::class, fn () => Client::connect($endpoint, '', 5.0));
    assertSame(StatusCode::INVALID_ARGUMENT, $e->status);
});

// A stranger who connects and says nothing must not keep the application out (ADR-0038 section 5).
$silent = stream_socket_client($endpoint, $errno, $error, 5) ?: fail("connect: $error");

$client = Client::connect($endpoint, $token, 5.0);
$worker = new Worker($client);
$st = new stdClass();
$st->connections = [];
$st->received = 0;
$worker->on('open', function (Connection $c) use ($st) {
    $st->connections[$c->id] = true;
})->on('message', function (Connection $c, string $data, bool $binary) use ($st) {
    $st->received++;
    $binary ? $c->sendBinary($data) : $c->send($data);
});
$until = function (callable $done, string $what) use ($worker): void {
    $deadline = microtime(true) + 10;
    while (!$done()) {
        microtime(true) < $deadline || fail("timed out waiting for $what");
        $worker->step(0.5);
    }
};

$check('the application gets in although a silent connection came first', function () use ($client) {
    assertTrue($client->maxFrame() > 0, 'HELLO_OK received over TCP');
});

$ws = ws_connect($wsPort);
$until(fn () => $st->connections !== [], 'the open event');

$check('a browser message is echoed through the TCP IPC link', function () use ($ws, $st, $until) {
    $before = $st->received;
    ws_send($ws, 1, 'hello over tcp');
    $until(fn () => $st->received > $before, 'the message event');
    assertSame([1, 'hello over tcp'], ws_read($ws));
});

$check('a second authenticated application is refused with BACKPRESSURE, the first undisturbed', function () use ($endpoint, $token, $ws, $st, $until) {
    $e = assertThrows(HandshakeRefused::class, fn () => Client::connect($endpoint, $token, 5.0));
    assertSame(StatusCode::BACKPRESSURE, $e->status);
    $before = $st->received;
    ws_send($ws, 1, 'still here');
    $until(fn () => $st->received > $before, 'the message event');
    assertSame([1, 'still here'], ws_read($ws));
});

$check('a 100 KiB binary message survives the TCP link in both directions', function () use ($ws, $st, $until) {
    $big = random_bytes(100 * 1024);
    $before = $st->received;
    ws_send($ws, 2, $big);
    $until(fn () => $st->received > $before, 'the message event');
    [$op, $payload] = ws_read($ws);
    assertSame(2, $op);
    assertTrue($payload === $big, 'payload identical');
});

fclose($silent);
fclose($ws);
proc_terminate($process);
proc_close($process);
foreach (glob($dir . '/*') ?: [] as $file) {
    @unlink($file);
}
@rmdir($dir);

if ($failures !== []) {
    fwrite(STDERR, count($failures) . " failed: " . implode(', ', $failures) . "\n");
    exit(1);
}
echo "all ok\n";
