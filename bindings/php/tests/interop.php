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
use Wsrv\Upgrade;
use Wsrv\Worker;

require __DIR__ . '/ws_client.php';

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

// ---- the handshake hook against the real daemon (ADR-0018) -----------------------------------

$hookDir = sys_get_temp_dir() . '/wsrv-php-hook-' . bin2hex(random_bytes(4));
mkdir($hookDir, 0700) || fail('mkdir');
$hookSocket = $hookDir . '/ipc.sock';
$hookProcess = proc_open(
    [$daemonPath, "--ipc-socket=$hookSocket", '--listen-port=0', '--allow-origin=https://app.example.com',
        '--shutdown-grace-ms=1500', '--handshake-hook=true', '--handshake-forward-header=Cookie'],
    [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['pipe', 'w']],
    $hookPipes
) ?: fail('cannot start the second wsrvd');
$hookPort = 0;
$hookLog = '';
$deadline = microtime(true) + 10;
while ($hookPort === 0 && microtime(true) < $deadline) {
    $r = [$hookPipes[2]];
    $w = $e = null;
    if (stream_select($r, $w, $e, 0, 200000) > 0) {
        $line = fgets($hookPipes[2]);
        $hookLog .= (string) $line;
        if ($line !== false && preg_match('/listening on port (\d+)/', $line, $m)) {
            $hookPort = (int) $m[1];
        }
    }
}
$hookPort > 0 || fail("the hook wsrvd did not start:\n$hookLog");

$check('hook: with no application connected an upgrade is refused with 503 at once', function () use ($hookPort) {
    [$s, $head] = ws_request($hookPort, "Cookie: session=good\r\n");
    assertTrue(str_starts_with($head, 'HTTP/1.1 503'), 'got: ' . strtok($head, "\r\n"));
    fclose($s);
});

$hookClient = Client::connect($hookSocket, '', 5.0);
$hookWorker = new Worker($hookClient);
$hk = new stdClass();
$hk->seen = [];     // [path, cookie] per question
$hk->opened = [];   // ids of opened connections
$hk->received = 0;  // message events
$hookWorker->on('authenticate', function (Upgrade $u) use ($hk) {
    $hk->seen[] = [$u->request->path, $u->request->cookie('session')];
    return match ($u->request->cookie('session')) {
        'good' => true,
        'expired' => 401,
        'slow' => null,   // answered later by the test
        default => false,
    };
})->on('open', function (Connection $c) use ($hk) {
    $hk->opened[$c->id] = true;
})->on('message', function (Connection $c, string $data) use ($hk) {
    $hk->received++;
    $c->send($data);
});

$hookUntil = function (callable $done, string $what) use ($hookWorker): void {
    $deadline = microtime(true) + 10;
    while (!$done()) {
        microtime(true) < $deadline || fail("timed out waiting for $what");
        $hookWorker->step(0.2);
    }
};

$check('hook: a good cookie is let in and the connection works', function () use ($hookPort, $hookUntil, $hk) {
    // The worker must run while the daemon waits for it: send the request, then dispatch events.
    $pending = ws_request_async($hookPort, "Cookie: theme=dark; session=good\r\n");
    $hookUntil(fn () => $hk->opened !== [], 'the open event');
    [$s, $head] = ws_finish($pending);
    assertTrue(str_starts_with($head, 'HTTP/1.1 101'), 'got: ' . strtok($head, "\r\n"));
    ws_send($s, 1, 'hello');
    $hookUntil(fn () => $hk->received > 0, 'the message event');
    assertSame([1, 'hello'], ws_read($s));
    assertSame(['/chat', 'good'], $hk->seen[0]);
    fclose($s);
});

$check('hook: an expired session is refused with 401 and never becomes a connection', function () use ($hookPort, $hookUntil, $hk) {
    $opened = count($hk->opened);
    $questions = count($hk->seen);
    $done = false;
    $tcp = ws_request_async($hookPort, "Cookie: session=expired\r\n");
    $hookUntil(fn () => count($hk->seen) > $questions, 'the question');
    [$s, $head] = ws_finish($tcp);
    assertTrue(str_starts_with($head, 'HTTP/1.1 401'), 'got: ' . strtok($head, "\r\n"));
    assertSame($opened, count($hk->opened), 'no open event for a refused upgrade');
    fclose($s);
});

$check('hook: no cookie at all is refused with 403', function () use ($hookPort, $hookUntil, $hk) {
    $questions = count($hk->seen);
    $tcp = ws_request_async($hookPort);
    $hookUntil(fn () => count($hk->seen) > $questions, 'the question');
    [$s, $head] = ws_finish($tcp);
    assertTrue(str_starts_with($head, 'HTTP/1.1 403'), 'got: ' . strtok($head, "\r\n"));
    fclose($s);
});

$check('hook: the application may answer later, through the Upgrade it kept', function () use ($hookPort, $hookWorker, $hookUntil, $hk) {
    $held = null;
    $hookWorker->on('authenticate', function (Upgrade $u) use (&$held) {
        $held = $u;
        return null;
    });
    $tcp = ws_request_async($hookPort, "Cookie: session=slow\r\n");
    $hookUntil(function () use (&$held) {
        return $held !== null;
    }, 'the question');
    $held->accept();
    [$s, $head] = ws_finish($tcp);
    assertTrue(str_starts_with($head, 'HTTP/1.1 101'), 'got: ' . strtok($head, "\r\n"));
    fclose($s);
});

$check('hook: when the application dies, the waiting upgrade is refused with 503 at once', function () use ($hookPort, $hookWorker, $hookClient, $hookUntil) {
    $held = false;
    $hookWorker->on('authenticate', function () use (&$held) {
        $held = true;
        return null;  // never answered
    });
    $tcp = ws_request_async($hookPort, "Cookie: session=gone\r\n");
    $hookUntil(function () use (&$held) {
        return $held;
    }, 'the question');
    $hookClient->close();
    [$s, $head] = ws_finish($tcp);
    assertTrue(str_starts_with($head, 'HTTP/1.1 503'), 'got: ' . strtok($head, "\r\n"));
    fclose($s);
});

proc_terminate($hookProcess, SIGTERM);
$deadline = microtime(true) + 10;
while (proc_get_status($hookProcess)['running'] && microtime(true) < $deadline) {
    usleep(50000);
}
@unlink($hookSocket);
@rmdir($hookDir);

// ---- rooms and the HTTP publish endpoint against the real daemon (ADR-0028) -------------------

use Wsrv\PublishFailed;
use Wsrv\Publisher;

$roomDir = sys_get_temp_dir() . '/wsrv-php-room-' . bin2hex(random_bytes(4));
mkdir($roomDir, 0700) || fail('mkdir');
$roomSocket = $roomDir . '/ipc.sock';
$tokenFile = $roomDir . '/publish.token';
// ADR-0039: the publish token must be one the daemon made. A token a person chose (here 48 hex
// characters, what the 1.0 guides told people to make) is refused at start, with a message that
// names the fix and does not print the token.
$chosenFile = $roomDir . '/chosen.token';
file_put_contents($chosenFile, bin2hex(random_bytes(24)) . "\n");
chmod($chosenFile, 0600);
exec(escapeshellarg($daemonPath) . ' --ipc-socket=' . escapeshellarg($roomDir . '/refused.sock')
    . ' --listen-port=0 --broker=true --publish-port=0 --publish-token-file=' . escapeshellarg($chosenFile) . ' 2>&1', $refusedOut, $refusedCode);
$refusedCode !== 0 || fail('wsrvd started with a publish token that --generate-token did not make');
str_contains(implode("\n", $refusedOut), '--generate-token') || fail('the refusal does not name --generate-token');
exec(escapeshellarg($daemonPath) . ' --generate-token=' . escapeshellarg($tokenFile) . ' 2>&1', $genOut, $genCode);
$genCode === 0 || fail('--generate-token failed: ' . implode(' ', $genOut));
$publishToken = trim((string) file_get_contents($tokenFile));
preg_match('/^wsrv1_[0-9a-f]{64}_[0-9a-f]{8}$/', $publishToken) === 1 || fail('the generated token has an unexpected form');
@unlink($chosenFile);
$roomProcess = proc_open(
    [$daemonPath, "--ipc-socket=$roomSocket", '--listen-port=0', '--allow-origin=https://app.example.com',
        '--shutdown-grace-ms=1500', '--broker=true', '--publish-port=0', "--publish-token-file=$tokenFile"],
    [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['pipe', 'w']],
    $roomPipes
) ?: fail('cannot start the third wsrvd');
$roomPort = 0;
$publishPort = 0;
$roomLog = '';
$deadline = microtime(true) + 10;
while (($roomPort === 0 || $publishPort === 0) && microtime(true) < $deadline) {
    $r = [$roomPipes[2]];
    $w = $e = null;
    if (stream_select($r, $w, $e, 0, 200000) > 0) {
        $line = fgets($roomPipes[2]);
        $roomLog .= (string) $line;
        if ($line !== false && preg_match('/event=listening .*listening on port (\d+)/', $line, $m)) {
            $roomPort = (int) $m[1];
        }
        if ($line !== false && preg_match('/event=publish_listening .*:(\d+)"/', $line, $m)) {
            $publishPort = (int) $m[1];
        }
    }
}
($roomPort > 0 && $publishPort > 0) || fail("the room wsrvd did not start:\n$roomLog");

$roomClient = Client::connect($roomSocket, '', 5.0);
$roomWorker = new Worker($roomClient);
$rm = new stdClass();
$rm->open = [];      // ids, in the order the browsers connected
$rm->statuses = [];  // request => status
$roomWorker->on('open', function (Connection $c) use ($rm) {
    $rm->open[] = $c;
})->on('status', function (int $request, int $status) use ($rm) {
    $rm->statuses[$request] = $status;
});
$roomUntil = function (callable $done, string $what) use ($roomWorker): void {
    $deadline = microtime(true) + 10;
    while (!$done()) {
        microtime(true) < $deadline || fail("timed out waiting for $what");
        $roomWorker->step(0.2);
    }
};
$statusOf = function (int $request) use ($rm, $roomUntil): int {
    $roomUntil(fn () => isset($rm->statuses[$request]), "the status of request $request");
    return $rm->statuses[$request];
};

$wsA = ws_connect($roomPort);
$wsB = ws_connect($roomPort);
$roomUntil(fn () => count($rm->open) === 2, 'two open events');
[$connA, $connB] = $rm->open;
$publisher = new Publisher("http://127.0.0.1:$publishPort", $publishToken);

$check('rooms: subscribe is answered OK, and a topic nobody joined reaches nobody', function () use ($connA, $connB, $statusOf, $publisher) {
    assertSame(StatusCode::OK, $statusOf($connA->subscribe('room.1')));
    assertSame(StatusCode::OK, $statusOf($connB->subscribe('room.1')));
    $result = $publisher->publish('empty.room', 'x');
    assertSame([0, 0, 0], [$result->subscribers, $result->accepted, $result->refused]);
});

$check('rooms: an HTTP publish reaches every subscriber, text and binary', function () use ($publisher, $wsA, $wsB) {
    $result = $publisher->publish('room.1', "h\u{e9}llo room");
    assertSame([2, 2, 0], [$result->subscribers, $result->accepted, $result->refused]);
    assertSame([1, "h\u{e9}llo room"], ws_read($wsA));
    assertSame([1, "h\u{e9}llo room"], ws_read($wsB));
    $publisher->publish('room.1', "\xff\x00\xfe", true);
    assertSame([2, "\xff\x00\xfe"], ws_read($wsA));
    assertSame([2, "\xff\x00\xfe"], ws_read($wsB));
});

$check('rooms: except leaves the sender out', function () use ($publisher, $connA, $wsA, $wsB) {
    $result = $publisher->publish('room.1', 'not for a', false, $connA->id);
    assertSame([1, 1, 0], [$result->subscribers, $result->accepted, $result->refused]);
    assertSame([1, 'not for a'], ws_read($wsB));
    stream_set_timeout($wsA, 0, 300000);
    assertSame('', (string) fread($wsA, 1), 'A heard nothing');
    stream_set_timeout($wsA, 5);
});

$check('rooms: the application can publish over IPC too, and unsubscribe', function () use ($roomWorker, $connA, $connB, $statusOf, $wsA, $wsB) {
    assertSame(StatusCode::OK, $statusOf($roomWorker->publish('room.1', 'from the worker', false, $connB->id)));
    assertSame([1, 'from the worker'], ws_read($wsA));
    assertSame(StatusCode::OK, $statusOf($connA->unsubscribe('room.1')));
    assertSame(StatusCode::OK, $statusOf($roomWorker->publish('room.1', 'only b', false, 0)));
    assertSame([1, 'only b'], ws_read($wsB));
});

$check('rooms: bad requests are refused with their status, never delivered', function () use ($publisher, $connA, $statusOf, $roomClient, $publishPort) {
    assertSame(StatusCode::NO_SUCH_CONN, $statusOf($roomClient->subscribe(999999, 'room.1')));
    assertSame(StatusCode::INVALID_UTF8, $statusOf($roomClient->publish('room.1', "\xff\xfe")));
    $e = assertThrows(PublishFailed::class, fn () => $publisher->publish('room.1', "\xff\xfe"));
    assertSame(415, $e->status);
    $wrong = new Publisher("http://127.0.0.1:$publishPort", str_repeat('x', 48));
    $e = assertThrows(PublishFailed::class, fn () => $wrong->publish('room.1', 'x'));
    assertSame(401, $e->status);
    assertSame(false, $e->isRetryable());
});

$check('rooms: a closed browser leaves its rooms (no orphans)', function () use ($publisher, $wsB) {
    fclose($wsB);
    $deadline = microtime(true) + 5;
    do {
        $result = $publisher->publish('room.1', 'anyone?');
        if ($result->subscribers === 0) {
            break;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    assertSame(0, $result->subscribers);
});

fclose($wsA);
proc_terminate($roomProcess, SIGTERM);
$deadline = microtime(true) + 10;
while (proc_get_status($roomProcess)['running'] && microtime(true) < $deadline) {
    usleep(50000);
}
@unlink($tokenFile);
@unlink($roomSocket);
@rmdir($roomDir);

@unlink($socket);
@rmdir($dir);
echo count($failures) === 0 ? "all interop checks passed\n" : count($failures) . " interop check(s) failed\n";
exit($failures === [] ? 0 : 1);
