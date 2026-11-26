<?php
// SPDX-License-Identifier: Apache-2.0
// End to end (ADR-0042): the REAL wsrvd, a Wsrv\Worker running the Pusher Gateway, a Wsrv\Publisher
// standing in for Laravel's broadcast, and raw WebSocket clients speaking the Pusher protocol.
// Run with: php tests/pusher_interop.php /path/to/wsrvd

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';
require __DIR__ . '/ws_client.php';
require __DIR__ . '/wsrvd_process.php';

use Wsrv\Client;
use Wsrv\Publisher;
use Wsrv\Pusher\Gateway;
use Wsrv\Pusher\GatewayConfig;
use Wsrv\Pusher\Signature;
use Wsrv\Pusher\SocketId;
use Wsrv\Worker;

const KEY = 'interop-key-0123';
const SECRET = 'interop-secret-0123456789abcdef';

$daemonPath = $argv[1] ?? fail('usage: php tests/pusher_interop.php /path/to/wsrvd');
$dir = sys_get_temp_dir() . '/wsrv-pusher-' . bin2hex(random_bytes(4));
mkdir($dir, 0700) || fail('mkdir');
$socket = $dir . '/ipc.sock';
$tokenFile = $dir . '/publish.token';
exec(escapeshellarg($daemonPath) . ' --generate-token=' . escapeshellarg($tokenFile) . ' 2>&1', $out, $code);
$code === 0 || fail('--generate-token failed');
$token = trim((string) file_get_contents($tokenFile));

$daemon = start_wsrvd($daemonPath, $dir, 'ipc', $tokenFile);
$process = $daemon['process'];
$port = $daemon['port'];
$publishPort = $daemon['publishPort'];
$socket = $daemon['socket'];

$worker = new Worker(Client::connect($socket, '', 5.0));
$gateway = Gateway::forWorker(new GatewayConfig(KEY, SECRET, maxPresenceMembers: 3, maxChannelsPerConnection: 4), $worker);
$publisher = new Publisher("http://127.0.0.1:$publishPort", $token);

/** Run the worker for a moment so that everything in flight is handled. */
$settle = function (float $seconds = 0.25) use ($worker): void {
    $end = microtime(true) + $seconds;
    while (microtime(true) < $end) {
        $worker->step(0.05);
    }
};

/** A browser: a raw socket and the Pusher frames it sees. */
$browser = function () use ($port, $settle): array {
    $ws = ws_connect($port, '', '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false');
    $settle();
    $hello = pusher_read($ws);
    assertSame('pusher:connection_established', $hello['event']);
    return [$ws, $hello['data']['socket_id']];
};

/** @param resource $ws */
function pusher_read($ws): array
{
    [$opcode, $payload] = ws_read($ws);
    assertSame(1, $opcode, 'a text message');
    $f = json_decode($payload, true, 16, JSON_THROW_ON_ERROR);
    if (isset($f['data']) && is_string($f['data'])) {
        $f['data'] = json_decode($f['data'], true, 16, JSON_THROW_ON_ERROR);
    }
    return $f;
}

/** @param resource $ws */
function pusher_send($ws, string $event, array $data, ?string $channel = null): void
{
    $f = ['event' => $event, 'data' => $data];
    if ($channel !== null) {
        $f['channel'] = $channel;
    }
    ws_send($ws, 1, json_encode($f, JSON_THROW_ON_ERROR));
}

function sign(string $socketId, string $channel, ?string $channelData = null): string
{
    return Signature::authorize(KEY, SECRET, $socketId, $channel, $channelData);
}

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

[$a, $sockA] = $browser();
[$b, $sockB] = $browser();

$check('the socket id names the connection the daemon knows', function () use ($sockA, $sockB) {
    $ca = SocketId::connection($sockA);
    $cb = SocketId::connection($sockB);
    assertTrue($ca !== null && $cb !== null && $ca !== $cb, "$sockA $sockB");
});

$check('ping is answered with pong', function () use ($a, $settle) {
    ws_send($a, 1, '{"event":"pusher:ping"}');
    $settle();
    assertSame('pusher:pong', pusher_read($a)['event']);
});

$check('a public channel: subscribe, then a Laravel-style broadcast arrives', function () use ($a, $settle, $publisher) {
    pusher_send($a, 'pusher:subscribe', ['channel' => 'orders']);
    $settle();
    $ok = pusher_read($a);
    assertSame('pusher_internal:subscription_succeeded', $ok['event']);
    assertSame('orders', $ok['channel']);
    $frame = json_encode(['event' => 'OrderShipped', 'data' => json_encode(['id' => 7]), 'channel' => 'orders']);
    $result = $publisher->publish('orders', $frame);
    assertSame(1, $result->subscribers);
    $got = pusher_read($a);
    assertSame('OrderShipped', $got['event']);
    assertSame(['id' => 7], $got['data']);
});

$check('a private channel needs the right signature', function () use ($a, $b, $sockA, $sockB, $settle) {
    pusher_send($b, 'pusher:subscribe', ['channel' => 'private-chat.1', 'auth' => sign($sockA, 'private-chat.1')]);
    $settle();
    $err = pusher_read($b);
    assertSame('pusher:error', $err['event']);
    assertSame(4009, $err['data']['code']);
    pusher_send($a, 'pusher:subscribe', ['channel' => 'private-chat.1', 'auth' => sign($sockA, 'private-chat.1')]);
    pusher_send($b, 'pusher:subscribe', ['channel' => 'private-chat.1', 'auth' => sign($sockB, 'private-chat.1')]);
    $settle();
    assertSame('pusher_internal:subscription_succeeded', pusher_read($a)['event']);
    assertSame('pusher_internal:subscription_succeeded', pusher_read($b)['event']);
});

$check('a whisper reaches the other member and not the sender', function () use ($a, $b, $settle) {
    pusher_send($a, 'client-typing', ['who' => 'ada'], 'private-chat.1');
    $settle();
    $got = pusher_read($b);
    assertSame('client-typing', $got['event']);
    assertSame('private-chat.1', $got['channel']);
    assertSame(['who' => 'ada'], $got['data']);
    assertTrue(!isset($got['user_id']), 'no user id on a private channel');
    // A had nothing queued: the next thing it sees is the answer to its ping.
    ws_send($a, 1, '{"event":"pusher:ping"}');
    $settle();
    assertSame('pusher:pong', pusher_read($a)['event']);
});

$check('toOthers: a broadcast keeps the sender out through its socket id', function () use ($a, $b, $sockA, $publisher, $settle) {
    $frame = json_encode(['event' => 'Msg', 'data' => '{}', 'channel' => 'private-chat.1']);
    $result = $publisher->publish('private-chat.1', $frame, false, SocketId::connection($sockA));
    assertSame(1, $result->subscribers);
    assertSame('Msg', pusher_read($b)['event']);
});

$check('presence: members, member_added, member_removed over a real daemon', function () use ($port, $a, $b, $sockA, $sockB, $settle) {
    $dataA = json_encode(['user_id' => '1', 'user_info' => ['name' => 'Ada']]);
    $dataB = json_encode(['user_id' => '2', 'user_info' => ['name' => 'Bob']]);
    pusher_send($a, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockA, 'presence-room', $dataA), 'channel_data' => $dataA]);
    $settle();
    $ok = pusher_read($a);
    assertSame(1, $ok['data']['presence']['count']);
    pusher_send($b, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockB, 'presence-room', $dataB), 'channel_data' => $dataB]);
    $settle();
    $added = pusher_read($a);
    assertSame('pusher_internal:member_added', $added['event']);
    assertSame(['user_id' => '2', 'user_info' => ['name' => 'Bob']], $added['data']);
    $okB = pusher_read($b);
    assertSame(2, $okB['data']['presence']['count']);
    assertSame(['name' => 'Ada'], $okB['data']['presence']['hash']['1']);

    [$c, ] = [ws_connect($port, '', '/app/' . KEY), null];
    $settle();
    pusher_read($c);                                            // connection_established
    fclose($c);                                                 // a browser that is not a member just goes away
    $settle();

    pusher_send($b, 'pusher:unsubscribe', ['channel' => 'presence-room']);
    $settle();
    $removed = pusher_read($a);
    assertSame('pusher_internal:member_removed', $removed['event']);
    assertSame(['user_id' => '2'], $removed['data']);
});

$check('a closed browser is removed from presence and told to nobody', function () use ($a, $sockA, $port, $settle) {
    [$d, $sockD] = [ws_connect($port, '', '/app/' . KEY), null];
    $settle();
    $hello = pusher_read($d);
    $sockD = $hello['data']['socket_id'];
    $dataD = json_encode(['user_id' => '9']);
    pusher_send($d, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockD, 'presence-room', $dataD), 'channel_data' => $dataD]);
    $settle();
    assertSame('pusher_internal:subscription_succeeded', pusher_read($d)['event']);
    assertSame('pusher_internal:member_added', pusher_read($a)['event']);
    fclose($d);
    $settle(0.5);
    $removed = pusher_read($a);
    assertSame('pusher_internal:member_removed', $removed['event']);
    assertSame(['user_id' => '9'], $removed['data']);
});

$check('hostile input: bad frames are answered, strikes close the connection, the daemon stays up', function () use ($port, $settle) {
    $x = ws_connect($port, '', '/app/' . KEY);
    $settle();
    pusher_read($x);
    for ($i = 0; $i < 8; $i++) {
        ws_send($x, 1, 'not json ' . $i);
    }
    $settle(0.5);
    $errors = 0;
    $closed = false;
    for ($i = 0; $i < 12 && !$closed; $i++) {
        [$op, $payload] = ws_read($x);
        if ($op === 8) {
            $closed = true;
            assertSame(1008, unpack('n', substr($payload, 0, 2))[1]);
        } else {
            $errors++;
        }
    }
    assertTrue($closed, 'closed with 1008');
    assertSame(8, $errors);
    fclose($x);
});

$check('a message over the daemon size limit closes only that connection', function () use ($port, $a, $settle) {
    $y = ws_connect($port, '', '/app/' . KEY);
    $settle();
    pusher_read($y);
    ws_send($y, 1, str_repeat('x', 20000));
    $settle();
    [$op, $payload] = ws_read($y);
    assertSame(8, $op);
    assertSame(1009, unpack('n', substr($payload, 0, 2))[1]);
    fclose($y);
    ws_send($a, 1, '{"event":"pusher:ping"}');
    $settle();
    assertSame('pusher:pong', pusher_read($a)['event']);
});

$check('channel quota: the fifth channel is refused with LimitReached', function () use ($a, $settle) {
    // a already holds orders, private-chat.1 and presence-room; one more fits
    pusher_send($a, 'pusher:subscribe', ['channel' => 'c1']);
    $settle();
    assertSame('pusher_internal:subscription_succeeded', pusher_read($a)['event']);
    pusher_send($a, 'pusher:subscribe', ['channel' => 'c2']);
    $settle();
    $f = pusher_read($a);
    assertSame('pusher:subscription_error', $f['event']);
    assertSame('LimitReached', $f['data']['type']);
});

// ---- the app key in the URL, checked at the handshake (check_app_key, needs handshake_hook) ---------------

$strict = start_wsrvd($daemonPath, $dir, 'strict', $tokenFile, ['--handshake-hook=true']);
$strictWorker = new Worker(Client::connect($strict['socket'], '', 5.0));
Gateway::forWorker(new GatewayConfig(KEY, SECRET), $strictWorker, true);
$strictSettle = function (float $seconds = 0.25) use ($strictWorker): void {
    $end = microtime(true) + $seconds;
    while (microtime(true) < $end) {
        $strictWorker->step(0.05);
    }
};

$check('check_app_key: the right key (with the query pusher-js adds) is let in, a wrong or missing one is a 404', function () use ($strict, $strictSettle) {
    $good = ws_request_async($strict['port'], '', '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false');
    $strictSettle();
    [$stream, $head] = ws_finish($good);
    assertTrue(str_starts_with($head, 'HTTP/1.1 101'), 'right key: ' . strtok($head, "\r\n"));
    fclose($stream);
    foreach (['/app/another-key?protocol=7', '/', '/chat', '/app/' . KEY . 'x'] as $path) {
        $bad = ws_request_async($strict['port'], '', $path);
        $strictSettle();
        [$stream, $head] = ws_finish($bad);
        assertTrue(str_starts_with($head, 'HTTP/1.1 404'), "$path: " . strtok($head, "\r\n"));
        fclose($stream);
    }
});

// ---- a page that joins all the channels it may in one burst, with the default limits --------------------

$check('the defaults carry a page that subscribes to 64 channels at once, and the 65th is refused', function () use ($strict, $strictSettle) {
    // pusher-js sends every subscribe back to back on load and after each reconnect, before the daemon
    // has answered one. This is that, against a real daemon, with a GatewayConfig left at its defaults
    // (quota 64, which is also the daemon's default broker_max_topics_per_connection).
    // (This daemon runs the handshake hook, so its worker must be stepped while the upgrade waits.)
    $pending = ws_request_async($strict['port'], '', '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false');
    $strictSettle();
    [$ws, $head] = ws_finish($pending);
    assertTrue(str_starts_with($head, 'HTTP/1.1 101'), 'upgrade: ' . strtok($head, "\r\n"));
    $strictSettle();
    assertSame('pusher:connection_established', pusher_read($ws)['event']);
    for ($i = 0; $i < 64; $i++) {
        pusher_send($ws, 'pusher:subscribe', ['channel' => "burst.$i"]);
    }
    $strictSettle(1.0);
    $joined = [];
    for ($i = 0; $i < 64; $i++) {
        $f = pusher_read($ws);
        assertSame('pusher_internal:subscription_succeeded', $f['event'], "answer $i: " . json_encode($f));
        $joined[$f['channel']] = true;
    }
    assertSame(64, count($joined), 'every one of the 64 channels was answered once');
    pusher_send($ws, 'pusher:subscribe', ['channel' => 'burst.64']);
    $strictSettle();
    $f = pusher_read($ws);
    assertSame('pusher:subscription_error', $f['event']);
    assertSame('LimitReached', $f['data']['type']);
    pusher_send($ws, 'pusher:ping', []);
    $strictSettle();
    assertSame('pusher:pong', pusher_read($ws)['event'], 'the connection is still open and served');
    fclose($ws);
});

proc_terminate($strict['process'], SIGTERM);

fclose($a);
fclose($b);
proc_terminate($process, SIGTERM);
$deadline = microtime(true) + 10;
while (proc_get_status($process)['running'] && microtime(true) < $deadline) {
    usleep(50000);
}
@unlink($tokenFile);
@unlink($socket);
@rmdir($dir);
echo count($failures) === 0 ? "all pusher interop checks passed\n" : count($failures) . " pusher interop check(s) failed\n";
exit($failures === [] ? 0 : 1);
