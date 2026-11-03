<?php
// SPDX-License-Identifier: Apache-2.0
// End to end: the REAL wsrvd, a Wsrv\Worker, and a raw WebSocket client written here.
// Run with: php tests/interop.php /path/to/wsrvd

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

use Wsrv\Client;
use Wsrv\Connection;
use Wsrv\HandshakeRefused;
use Wsrv\StatusCode;
use Wsrv\Worker;

// ---- a minimal WebSocket client (RFC 6455), enough to drive the daemon ----------------------

/** @return resource */
function ws_connect(int $port)
{
    $s = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5) ?: fail("connect: $error");
    $key = base64_encode(random_bytes(16));
    fwrite($s, "GET /chat HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\nOrigin: https://app.example.com\r\n\r\n");
    stream_set_timeout($s, 5);
    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $byte = fread($s, 1);
        if ($byte === false || $byte === '') {
            fail('no HTTP response: ' . $head);
        }
        $head .= $byte;
    }
    str_starts_with($head, 'HTTP/1.1 101') || fail('handshake refused: ' . $head);
    $accept = base64_encode(sha1($key . '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', true));
    str_contains($head, $accept) || fail('wrong Sec-WebSocket-Accept');
    return $s;
}

/** @param resource $s */
function ws_send($s, int $opcode, string $payload): void
{
    $length = strlen($payload);
    $head = chr(0x80 | $opcode);
    if ($length < 126) {
        $head .= chr(0x80 | $length);
    } elseif ($length < 65536) {
        $head .= chr(0x80 | 126) . pack('n', $length);
    } else {
        $head .= chr(0x80 | 127) . pack('J', $length);
    }
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0; $i < $length; $i++) {
        $masked .= $payload[$i] ^ $mask[$i % 4];
    }
    fwrite($s, $head . $mask . $masked);
}

/** @param resource $s */
function ws_read_exact($s, int $n): string
{
    $out = '';
    while (strlen($out) < $n) {
        $chunk = fread($s, $n - strlen($out));
        if ($chunk === false || $chunk === '') {
            fail('connection ended while reading a frame');
        }
        $out .= $chunk;
    }
    return $out;
}

/**
 * One whole message: continuation frames are joined until FIN (RFC 6455 section 5.4; the daemon
 * fragments big outbound messages). Control frames between fragments are not expected here.
 *
 * @param resource $s
 * @return array{0:int, 1:string} [opcode of the first frame, payload]
 */
function ws_read($s): array
{
    $first = null;
    $payload = '';
    do {
        $h = ws_read_exact($s, 2);
        $fin = (ord($h[0]) & 0x80) !== 0;
        $opcode = ord($h[0]) & 0x0F;
        $length = ord($h[1]) & 0x7F;
        if ($length === 126) {
            $length = unpack('n', ws_read_exact($s, 2))[1];
        } elseif ($length === 127) {
            $length = unpack('J', ws_read_exact($s, 8))[1];
        }
        $payload .= ws_read_exact($s, $length);
        $first ??= $opcode;
        if ($first === 0 || ($opcode !== 0 && $opcode !== $first && $first <= 2)) {
            fail('unexpected frame sequence (first opcode ' . $first . ', then ' . $opcode . ')');
        }
    } while (!$fin);
    return [$first, $payload];
}

// ---- start the daemon -----------------------------------------------------------------------

$daemonPath = $argv[1] ?? fail('usage: php tests/interop.php /path/to/wsrvd');
$dir = sys_get_temp_dir() . '/wsrv-php-' . bin2hex(random_bytes(4));
mkdir($dir, 0700) || fail('mkdir');
$socket = $dir . '/ipc.sock';
$process = proc_open(
    [$daemonPath, "--ipc-socket=$socket", '--listen-port=0', '--allow-origin=https://app.example.com', '--shutdown-grace-ms=1500'],
    [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['pipe', 'w']],
    $pipes
) ?: fail('cannot start wsrvd');

$port = 0;
$log = '';
$deadline = microtime(true) + 10;
while ($port === 0 && microtime(true) < $deadline) {
    $r = [$pipes[2]];
    $w = $e = null;
    if (stream_select($r, $w, $e, 0, 200000) > 0) {
        $line = fgets($pipes[2]);
        $log .= (string) $line;
        if ($line !== false && preg_match('/listening on port (\d+)/', $line, $m)) {
            $port = (int) $m[1];
        }
    }
}
$port > 0 || fail("wsrvd did not start:\n$log");

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

$client = Client::connect($socket, '', 5.0);
$worker = new Worker($client);
$st = new stdClass();
$st->connections = [];  // id => true, from open events
$st->received = 0;     // message events seen
$st->statuses = [];    // request => status
$st->closed = [];      // [id, code, reason, clean]
$worker->on('open', function (Connection $c) use ($st) {
    $st->connections[$c->id] = true;
})->on('message', function (Connection $c, string $data, bool $binary) use ($st) {
    $st->received++;
    $binary ? $c->sendBinary($data) : $c->send($data);
});

/** Dispatch worker events until $done() holds (STATUS answers and the like are not what we wait for). */
$until = function (callable $done, string $what) use ($worker): void {
    $deadline = microtime(true) + 10;
    while (!$done()) {
        microtime(true) < $deadline || fail("timed out waiting for $what");
        $worker->step(0.5);
    }
};

$ws = ws_connect($port);
$until(fn () => $st->connections !== [], 'the open event');

$pump = function (int $messages) use ($st, $until): void {
    $target = $st->received + $messages;
    $until(fn () => $st->received >= $target, 'a message event');
};

$check('text echo', function () use ($ws, $pump) {
    ws_send($ws, 1, 'hello');
    $pump(1);
    assertSame([1, 'hello'], ws_read($ws));
});

$check('multibyte UTF-8 text', function () use ($ws, $pump) {
    ws_send($ws, 1, "h\u{e9}llo \u{2713} \u{1F600}");
    $pump(1);
    assertSame([1, "h\u{e9}llo \u{2713} \u{1F600}"], ws_read($ws));
});

$check('binary with every byte value', function () use ($ws, $pump) {
    $all = implode('', array_map('chr', range(0, 255)));
    ws_send($ws, 2, $all);
    $pump(1);
    assertSame([2, $all], ws_read($ws));
});

$check('100 KiB message (extended length framing)', function () use ($ws, $pump) {
    $big = random_bytes(100 * 1024);
    ws_send($ws, 2, $big);
    $pump(1);
    [$op, $payload] = ws_read($ws);
    assertSame(2, $op);
    assertTrue($payload === $big, 'payload identical');
});

$check('empty text message', function () use ($ws, $pump) {
    ws_send($ws, 1, '');
    $pump(1);
    assertSame([1, ''], ws_read($ws));
});

$check('a second application is refused with BACKPRESSURE', function () use ($socket) {
    $e = assertThrows(HandshakeRefused::class, fn () => Client::connect($socket, '', 5.0));
    assertSame(StatusCode::BACKPRESSURE, $e->status);
});

$worker->on('status', function (int $request, int $status) use ($st) {
    $st->statuses[$request] = $status;
});

$check('invalid UTF-8 from the application is rejected, not sent', function () use ($client, $until, $st) {
    $request = $client->sendText(array_key_first($st->connections), "\xff\xfe");
    $until(fn () => isset($st->statuses[$request]), 'the status');
    assertSame(StatusCode::INVALID_UTF8, $st->statuses[$request]);
});

$check('a command for a connection that does not exist is NO_SUCH_CONN', function () use ($client, $until, $st) {
    $request = $client->sendText(987654, 'x');
    $until(fn () => isset($st->statuses[$request]), 'the status');
    assertSame(StatusCode::NO_SUCH_CONN, $st->statuses[$request]);
});

$worker->on('close', function (Connection $c, int $code, string $reason, bool $clean) use ($st) {
    $st->closed[] = [$c->id, $code, $reason, $clean];
});

$check('the application closes a connection with its own code', function () use ($ws, $worker, $client, $until, $st) {
    $id = array_key_first($st->connections);
    $client->closeConnection($id, 4001, 'done');
    [$op, $payload] = ws_read($ws);
    assertSame(8, $op, 'close frame');
    assertSame(4001, unpack('n', $payload)[1]);
    assertSame('done', substr($payload, 2));
    ws_send($ws, 8, pack('n', 4001) . 'ack');  // the browser's reply carries ITS reason
    $until(fn () => $st->closed !== [], 'the close event');
    assertSame([[$id, 4001, 'ack', true]], $st->closed);  // the close event reports the peer's code and reason
    assertSame(0, $worker->connectionCount());
});

$check('the browser closing first is reported to the application', function () use ($port, $until, $st) {
    $st->closed = [];
    $known = count($st->connections);
    $ws2 = ws_connect($port);
    $until(fn () => count($st->connections) > $known, 'the open event');
    ws_send($ws2, 8, pack('n', 1000) . 'bye');
    ws_read($ws2);  // the daemon's close reply
    $until(fn () => $st->closed !== [], 'the close event');
    assertSame([1000, 'bye', true], array_slice($st->closed[0], 1));
});

$check('a browser that vanishes is reported as an abnormal close', function () use ($port, $until, $st) {
    $st->closed = [];
    $known = count($st->connections);
    $ws3 = ws_connect($port);
    $until(fn () => count($st->connections) > $known, 'the open event');
    fclose($ws3);  // no closing handshake at all
    $until(fn () => $st->closed !== [], 'the close event');
    assertSame([1006, false], [$st->closed[0][1], $st->closed[0][3]]);
});

$check('the daemon stops cleanly on SIGTERM and removes its socket', function () use ($process, $socket) {
    proc_terminate($process, SIGTERM);
    $deadline = microtime(true) + 10;
    while (microtime(true) < $deadline) {
        $status = proc_get_status($process);
        if (!$status['running']) {
            assertSame(0, $status['exitcode'], 'exit code');
            assertTrue(!file_exists($socket), 'socket file removed');
            return;
        }
        usleep(50000);
    }
    fail('wsrvd did not stop within 10 seconds');
});

@unlink($socket);
@rmdir($dir);
echo count($failures) === 0 ? "all interop checks passed\n" : count($failures) . " interop check(s) failed\n";
exit($failures === [] ? 0 : 1);
