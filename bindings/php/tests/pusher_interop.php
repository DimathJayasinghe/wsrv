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
    $hello = pusher_read_pumped($ws);
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

/**
 * Run the worker until $ws has bytes to read, or fail after $seconds. A frame the worker has yet to produce is
 * waited for, not missed because a fixed stretch of stepping was too short on a loaded machine or a slow
 * sanitizer build: with nobody stepping the worker, a blocking read just waits for its timeout.
 * stream_select() does not see the bytes PHP has already buffered, hence unread_bytes.
 *
 * @param resource $ws
 */
function wait_readable($ws, Worker $worker, float $seconds = 10.0): void
{
    $deadline = microtime(true) + $seconds;
    while (stream_get_meta_data($ws)['unread_bytes'] === 0) {
        $read = [$ws];
        $write = null;
        $except = null;
        if (stream_select($read, $write, $except, 0, 0) > 0) {
            break;
        }
        assertTrue(microtime(true) < $deadline, "no frame within $seconds seconds");
        $worker->step(0.02);
    }
}

/**
 * Like pusher_read(), but runs the worker while it waits for the frame (see wait_readable()).
 *
 * @param resource $ws
 */
function pusher_read_when_ready($ws, Worker $worker, float $seconds = 10.0): array
{
    wait_readable($ws, $worker, $seconds);
    return pusher_read($ws);
}

/**
 * pusher_read_when_ready() for the test's main worker ($worker, which most checks use).
 *
 * @param resource $ws
 */
function pusher_read_pumped($ws): array
{
    global $worker;
    return pusher_read_when_ready($ws, $worker);
}

/**
 * ws_read() for the main worker: waits for the first byte while running it (a close frame, say).
 *
 * @param resource $ws
 * @return array{0:int, 1:string}
 */
function ws_read_pumped($ws): array
{
    global $worker;
    wait_readable($ws, $worker);
    return ws_read($ws);
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
    assertSame('pusher:pong', pusher_read_pumped($a)['event']);
});

$check('a public channel: subscribe, then a Laravel-style broadcast arrives', function () use ($a, $settle, $publisher) {
    pusher_send($a, 'pusher:subscribe', ['channel' => 'orders']);
    $settle();
    $ok = pusher_read_pumped($a);
    assertSame('pusher_internal:subscription_succeeded', $ok['event']);
    assertSame('orders', $ok['channel']);
    $frame = json_encode(['event' => 'OrderShipped', 'data' => json_encode(['id' => 7]), 'channel' => 'orders']);
    $result = $publisher->publish('orders', $frame);
    assertSame(1, $result->subscribers);
    $got = pusher_read_pumped($a);
    assertSame('OrderShipped', $got['event']);
    assertSame(['id' => 7], $got['data']);
});

$check('a private channel needs the right signature', function () use ($a, $b, $sockA, $sockB, $settle) {
    pusher_send($b, 'pusher:subscribe', ['channel' => 'private-chat.1', 'auth' => sign($sockA, 'private-chat.1')]);
    $settle();
    $err = pusher_read_pumped($b);
    assertSame('pusher:error', $err['event']);
    assertSame(4009, $err['data']['code']);
    pusher_send($a, 'pusher:subscribe', ['channel' => 'private-chat.1', 'auth' => sign($sockA, 'private-chat.1')]);
    pusher_send($b, 'pusher:subscribe', ['channel' => 'private-chat.1', 'auth' => sign($sockB, 'private-chat.1')]);
    $settle();
    assertSame('pusher_internal:subscription_succeeded', pusher_read_pumped($a)['event']);
    assertSame('pusher_internal:subscription_succeeded', pusher_read_pumped($b)['event']);
});

$check('a whisper reaches the other member and not the sender', function () use ($a, $b, $settle) {
    pusher_send($a, 'client-typing', ['who' => 'ada'], 'private-chat.1');
    $settle();
    $got = pusher_read_pumped($b);
    assertSame('client-typing', $got['event']);
    assertSame('private-chat.1', $got['channel']);
    assertSame(['who' => 'ada'], $got['data']);
    assertTrue(!isset($got['user_id']), 'no user id on a private channel');
    // A had nothing queued: the next thing it sees is the answer to its ping.
    ws_send($a, 1, '{"event":"pusher:ping"}');
    $settle();
    assertSame('pusher:pong', pusher_read_pumped($a)['event']);
});

$check('toOthers: a broadcast keeps the sender out through its socket id', function () use ($a, $b, $sockA, $publisher, $settle) {
    $frame = json_encode(['event' => 'Msg', 'data' => '{}', 'channel' => 'private-chat.1']);
    $result = $publisher->publish('private-chat.1', $frame, false, SocketId::connection($sockA));
    assertSame(1, $result->subscribers);
    assertSame('Msg', pusher_read_pumped($b)['event']);
});

$check('presence: members, member_added, member_removed over a real daemon', function () use ($port, $a, $b, $sockA, $sockB, $settle) {
    $dataA = json_encode(['user_id' => '1', 'user_info' => ['name' => 'Ada']]);
    $dataB = json_encode(['user_id' => '2', 'user_info' => ['name' => 'Bob']]);
    pusher_send($a, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockA, 'presence-room', $dataA), 'channel_data' => $dataA]);
    $settle();
    $ok = pusher_read_pumped($a);
    assertSame(1, $ok['data']['presence']['count']);
    pusher_send($b, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockB, 'presence-room', $dataB), 'channel_data' => $dataB]);
    $settle();
    $added = pusher_read_pumped($a);
    assertSame('pusher_internal:member_added', $added['event']);
    assertSame(['user_id' => '2', 'user_info' => ['name' => 'Bob']], $added['data']);
    $okB = pusher_read_pumped($b);
    assertSame(2, $okB['data']['presence']['count']);
    assertSame(['name' => 'Ada'], $okB['data']['presence']['hash']['1']);

    [$c, ] = [ws_connect($port, '', '/app/' . KEY), null];
    $settle();
    pusher_read_pumped($c);                                            // connection_established
    fclose($c);                                                 // a browser that is not a member just goes away
    $settle();

    pusher_send($b, 'pusher:unsubscribe', ['channel' => 'presence-room']);
    $settle();
    $removed = pusher_read_pumped($a);
    assertSame('pusher_internal:member_removed', $removed['event']);
    assertSame(['user_id' => '2'], $removed['data']);
});

$check('a closed browser is removed from presence and told to nobody', function () use ($a, $sockA, $port, $settle) {
    [$d, $sockD] = [ws_connect($port, '', '/app/' . KEY), null];
    $settle();
    $hello = pusher_read_pumped($d);
    $sockD = $hello['data']['socket_id'];
    $dataD = json_encode(['user_id' => '9']);
    pusher_send($d, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockD, 'presence-room', $dataD), 'channel_data' => $dataD]);
    $settle();
    assertSame('pusher_internal:subscription_succeeded', pusher_read_pumped($d)['event']);
    assertSame('pusher_internal:member_added', pusher_read_pumped($a)['event']);
    fclose($d);
    $settle(0.5);
    $removed = pusher_read_pumped($a);
    assertSame('pusher_internal:member_removed', $removed['event']);
    assertSame(['user_id' => '9'], $removed['data']);
});

$check('a repeated presence subscribe is answered with the members who are in now, also while the list is kept', function () use ($a, $sockA, $browser, $settle) {
    // The gateway keeps the member list it built for a repeat (ADR-0042 A2). It must never answer from a list
    // the members have outgrown: the room holds user 1 (this connection) when the check starts and ends.
    $dataA = json_encode(['user_id' => '1', 'user_info' => ['name' => 'Ada']]);
    $again = function () use ($a, $sockA, $dataA, $settle): array {
        pusher_send($a, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockA, 'presence-room', $dataA), 'channel_data' => $dataA]);
        $settle();
        $f = pusher_read_pumped($a);
        assertSame('pusher_internal:subscription_succeeded', $f['event']);
        assertSame('presence-room', $f['channel']);
        return $f['data']['presence']['ids'];
    };
    assertSame(['1'], $again());
    assertSame(['1'], $again(), 'asked twice, answered twice');
    [$e, $sockE] = $browser();
    $dataE = json_encode(['user_id' => '7']);
    pusher_send($e, 'pusher:subscribe', ['channel' => 'presence-room', 'auth' => sign($sockE, 'presence-room', $dataE), 'channel_data' => $dataE]);
    $settle();
    assertSame(['1', '7'], pusher_read_pumped($e)['data']['presence']['ids']);
    assertSame('pusher_internal:member_added', pusher_read_pumped($a)['event']);
    assertSame(['1', '7'], $again(), 'a member who came is in the answer');
    fclose($e);
    $settle(0.5);
    assertSame('pusher_internal:member_removed', pusher_read_pumped($a)['event']);
    assertSame(['1'], $again(), 'and out of it again when they left');
});

$check('hostile input: bad frames are answered, strikes close the connection, the daemon stays up', function () use ($port, $settle) {
    $x = ws_connect($port, '', '/app/' . KEY);
    $settle();
    pusher_read_pumped($x);
    for ($i = 0; $i < 8; $i++) {
        ws_send($x, 1, 'not json ' . $i);
    }
    $settle(0.5);
    $errors = 0;
    $closed = false;
    for ($i = 0; $i < 12 && !$closed; $i++) {
        [$op, $payload] = ws_read_pumped($x);
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
    pusher_read_pumped($y);
    ws_send($y, 1, str_repeat('x', 20000));
    $settle();
    [$op, $payload] = ws_read_pumped($y);
    assertSame(8, $op);
    assertSame(1009, unpack('n', substr($payload, 0, 2))[1]);
    fclose($y);
    ws_send($a, 1, '{"event":"pusher:ping"}');
    $settle();
    assertSame('pusher:pong', pusher_read_pumped($a)['event']);
});

$check('channel quota: the fifth channel is refused with LimitReached', function () use ($a, $settle) {
    // a already holds orders, private-chat.1 and presence-room; one more fits
    pusher_send($a, 'pusher:subscribe', ['channel' => 'c1']);
    $settle();
    assertSame('pusher_internal:subscription_succeeded', pusher_read_pumped($a)['event']);
    pusher_send($a, 'pusher:subscribe', ['channel' => 'c2']);
    $settle();
    $f = pusher_read_pumped($a);
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

$check('the defaults carry a page that subscribes to 64 channels at once, and the 65th is refused', function () use ($strict, $strictSettle, $strictWorker) {
    // pusher-js sends every subscribe back to back on load and after each reconnect, before the daemon
    // has answered one. This is that, against a real daemon, with a GatewayConfig left at its defaults
    // (quota 64, which is also the daemon's default broker_max_topics_per_connection).
    // (This daemon runs the handshake hook, so its worker must be stepped while the upgrade waits.)
    $pending = ws_request_async($strict['port'], '', '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false');
    $strictSettle();
    [$ws, $head] = ws_finish($pending);
    assertTrue(str_starts_with($head, 'HTTP/1.1 101'), 'upgrade: ' . strtok($head, "\r\n"));
    $strictSettle();
    assertSame('pusher:connection_established', pusher_read_when_ready($ws, $strictWorker)['event']);
    for ($i = 0; $i < 64; $i++) {
        pusher_send($ws, 'pusher:subscribe', ['channel' => "burst.$i"]);
    }
    $joined = [];
    for ($i = 0; $i < 64; $i++) {
        $f = pusher_read_when_ready($ws, $strictWorker);
        assertSame('pusher_internal:subscription_succeeded', $f['event'], "answer $i: " . json_encode($f));
        $joined[$f['channel']] = true;
    }
    assertSame(64, count($joined), 'every one of the 64 channels was answered once');
    pusher_send($ws, 'pusher:subscribe', ['channel' => 'burst.64']);
    $f = pusher_read_when_ready($ws, $strictWorker);
    assertSame('pusher:subscription_error', $f['event']);
    assertSame('LimitReached', $f['data']['type']);
    pusher_send($ws, 'pusher:ping', []);
    assertSame('pusher:pong', pusher_read_when_ready($ws, $strictWorker)['event'], 'the connection is still open and served');
    fclose($ws);
});

// ---- a member list that does not fit the daemon's IPC frame: that join fails, nothing else --------------

// A presence join is answered with the whole member list, so one member with a big user_info in a nearly
// full room could ask the worker to send more than the daemon accepts in one frame. That used to drop the
// link to the daemon, which closes every browser. A daemon with a 4 KiB frame makes it small enough to see.
$small = start_wsrvd($daemonPath, $dir, 'small', $tokenFile, ['--max-message-size=2048', '--ipc-max-frame=4096']);
$smallWorker = new Worker(Client::connect($small['socket'], '', 5.0));
$smallGateway = Gateway::forWorker(
    new GatewayConfig(KEY, SECRET, maxFrameBytes: 2048, maxPresenceMembers: 10, maxChannelDataBytes: 1200),
    $smallWorker
);

$check('a member list too large for the daemon frame fails that join alone: the link, the room and the other members are untouched', function () use ($small, $smallWorker, $smallGateway) {
    $step = function (float $seconds = 0.25) use ($smallWorker): void {
        $end = microtime(true) + $seconds;
        while (microtime(true) < $end) {
            $smallWorker->step(0.05);
        }
    };
    $path = '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false';
    $info = str_repeat('x', 800);   // about 860 bytes of the list per member: four fit in 4096, the fifth does not
    $joins = [];
    for ($i = 1; $i <= 6; $i++) {
        $ws = ws_connect($small['port'], '', $path);
        $step();
        $socketId = pusher_read_when_ready($ws, $smallWorker)['data']['socket_id'];
        $data = json_encode(['user_id' => "u$i", 'user_info' => ['bio' => $info]], JSON_THROW_ON_ERROR);
        pusher_send($ws, 'pusher:subscribe', [
            'channel' => 'presence-big', 'auth' => sign($socketId, 'presence-big', $data), 'channel_data' => $data,
        ]);
        $joins[$i] = [$ws, pusher_read_when_ready($ws, $smallWorker)];
    }
    $in = [];
    $refused = [];
    foreach ($joins as $i => [, $answer]) {
        if ($answer['event'] === 'pusher_internal:subscription_succeeded') {
            $in[] = $i;
        } else {
            assertSame('pusher:subscription_error', $answer['event'], "member $i: " . json_encode($answer));
            assertSame('ServerError', $answer['data']['type'], "member $i");
            assertSame('presence-big', $answer['channel'], "member $i");
            assertSame('The member list is too large to send', $answer['data']['error'], "member $i");
            $refused[] = $i;
        }
    }
    assertTrue(count($in) >= 2 && count($refused) >= 1, 'some members fit and some do not: in ' . json_encode($in) . ', refused ' . json_encode($refused));
    assertSame($in, range(1, count($in)), 'the first members were let in, in order');
    assertTrue(in_array(6, $refused, true), 'the last one is refused');

    // Everyone is still connected and served, refused or not.
    foreach ($joins as $i => [$ws]) {
        ws_send($ws, 1, '{"event":"pusher:ping"}');
        $seen = [];
        do {
            $f = pusher_read_when_ready($ws, $smallWorker);
            if ($f['event'] === 'pusher_internal:member_added') {
                $seen[] = $f['data']['user_id'];
            }
        } while ($f['event'] !== 'pusher:pong');
        if (in_array($i, $in, true)) {
            // told of the members who joined after it, never of one who was refused
            $expected = array_map(static fn (int $m): string => "u$m", array_values(array_filter($in, static fn (int $m): bool => $m > $i)));
            assertSame($expected, $seen, "member $i saw these members join");
        } else {
            assertSame([], $seen, "refused member $i saw nobody join");
        }
    }
    // The worker's link survived: a new browser is still answered, and the room has exactly the members who fit.
    $late = ws_connect($small['port'], '', $path);
    $step();
    assertSame('pusher:connection_established', pusher_read_when_ready($late, $smallWorker)['event']);
    $stats = $smallGateway->stats();
    assertSame(count($in), $stats->joins, 'only the joins that were answered are counted');
    assertSame(count($refused), $stats->subscriptionErrors);
    assertSame(0, $stats->daemonRefused, 'the daemon refused nothing: the worker did not send what could not fit');
    assertSame(1, $stats->presenceChannels);
    assertSame(count($in), $stats->presenceMembers);
    fclose($late);
    foreach ($joins as [$ws]) {
        fclose($ws);
    }
});

// ---- a member list that fits the IPC frame but not what the daemon can send to one browser -----------
// ADR-0043. The daemon's outbound queue per browser is 2 x max_message_size, and it used to drop a larger
// message and still answer OK, so a join whose member list was over that size (32752 bytes at the guide's
// --max-message-size=16384) got no answer at all, while the member was counted and announced. Now the
// frame limit the daemon announces is that size plus the frame's own fields, so the worker's client
// refuses the message before it is written, and the join fails the same clean way as above.
$queue = start_wsrvd($daemonPath, $dir, 'queue', $tokenFile, ['--max-message-size=16384', '--ipc-max-frame=524288']);
$queueClient = Client::connect($queue['socket'], '', 5.0);
$queueWorker = new Worker($queueClient);
$queueGateway = Gateway::forWorker(new GatewayConfig(KEY, SECRET, maxFrameBytes: 16384, maxChannelDataBytes: 2048), $queueWorker);

$check('the daemon announces a frame limit no larger than what it can send to one browser', function () use ($queueClient) {
    // 2 x 16384 - 16 (the queue's cost per message) + 14 (what a SEND frame carries besides the message)
    assertSame(2 * 16384 - 16 + 14, $queueClient->maxFrame(), 'the guide asked for 524288; nothing above 32766 could be delivered');
});

$check('a member list over what the daemon can send to a browser fails that join, with an answer, and the rest are untouched', function () use ($queue, $queueWorker, $queueGateway) {
    $step = function (float $seconds = 0.25) use ($queueWorker): void {
        $end = microtime(true) + $seconds;
        while (microtime(true) < $end) {
            $queueWorker->step(0.05);
        }
    };
    $path = '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false';
    $info = str_repeat('x', 900);   // about 1 KB of the list per member: about 31 fit in 32752 bytes
    $answers = [];
    $sockets = [];
    for ($i = 1; $i <= 36; $i++) {
        $ws = ws_connect($queue['port'], '', $path);
        $step(0.1);
        $socketId = pusher_read_when_ready($ws, $queueWorker)['data']['socket_id'];
        $data = json_encode(['user_id' => "u$i", 'user_info' => ['bio' => $info]], JSON_THROW_ON_ERROR);
        pusher_send($ws, 'pusher:subscribe', [
            'channel' => 'presence-q', 'auth' => sign($socketId, 'presence-q', $data), 'channel_data' => $data,
        ]);
        $sockets[$i] = $ws;
        // Every join must be answered, one way or the other: a silent drop would hang here until the stream's timeout.
        $answers[$i] = pusher_read_when_ready($ws, $queueWorker);
    }
    $in = [];
    $refused = [];
    foreach ($answers as $i => $answer) {
        if ($answer['event'] === 'pusher_internal:subscription_succeeded') {
            $in[] = $i;
            continue;
        }
        assertSame('pusher:subscription_error', $answer['event'], "member $i: " . json_encode($answer));
        assertSame('The member list is too large to send', $answer['data']['error'], "member $i");
        $refused[] = $i;
    }
    assertTrue(count($in) >= 20 && count($in) < 36, 'some fit and some do not: in ' . count($in));
    assertSame($in, range(1, count($in)), 'the first members were let in, in order');
    assertSame(range(count($in) + 1, 36), $refused, 'every join after the list outgrew the queue is refused');
    $stats = $queueGateway->stats();
    assertSame(count($in), $stats->joins, 'only the joins that were answered are counted');
    assertSame(count($in), $stats->presenceMembers, 'a refused member is not left in the room');
    assertSame(0, $stats->daemonRefused, 'the worker did not send what could not fit');
    foreach ($sockets as $ws) {
        fclose($ws);
    }
});

proc_terminate($queue['process'], SIGTERM);


// ---- anonymous public joins must not starve the signed joins of real users -----------------------------

// A public channel needs no signature, and every distinct name is a room (a topic) in the daemon's broker,
// whose table holds 1 024 by default. One connection may hold 64 names and one address 100 connections, so
// sixteen scripted connections could fill the table in a fraction of a second, and a user who then logged
// in was told "The server is at its channel limit" for their private and presence channels (ADR-0042 A3).
// The daemon and the gateway here are on their defaults.
$open = start_wsrvd($daemonPath, $dir, 'open', $tokenFile);
$openWorker = new Worker(Client::connect($open['socket'], '', 5.0));
$openGateway = Gateway::forWorker(new GatewayConfig(KEY, SECRET), $openWorker);

$check('sixteen anonymous connections joining 64 public names each cannot keep a signed user out of their channels', function () use ($open, $openWorker, $openGateway) {
    $path = '/app/' . KEY . '?protocol=7&client=js&version=8.4.0&flash=false';
    $step = function (float $seconds = 0.25) use ($openWorker): void {
        $end = microtime(true) + $seconds;
        while (microtime(true) < $end) {
            $openWorker->step(0.05);
        }
    };
    $connect = function () use ($open, $path, $step, $openWorker): array {
        $ws = ws_connect($open['port'], '', $path);
        $step();
        return [$ws, pusher_read_when_ready($ws, $openWorker)['data']['socket_id']];
    };
    $attackers = [];
    for ($a = 0; $a < 16; $a++) {
        [$ws] = $connect();
        for ($j = 0; $j < 64; $j++) {
            pusher_send($ws, 'pusher:subscribe', ['channel' => "flood.$a.$j"]);
        }
        $attackers[] = $ws;
    }
    $joined = 0;
    $refused = 0;
    foreach ($attackers as $ws) {
        for ($j = 0; $j < 64; $j++) {
            $f = pusher_read_when_ready($ws, $openWorker);
            if ($f['event'] === 'pusher_internal:subscription_succeeded') {
                $joined++;
            } else {
                assertSame('pusher:subscription_error', $f['event']);
                assertSame('LimitReached', $f['data']['type']);
                $refused++;
            }
        }
    }
    assertSame(1024, $joined + $refused, 'every one of the 16 x 64 joins was answered');
    // ADR-0042 A4: of the 512 names a quarter is kept for signed-in users, so the flood stops at 384.
    assertSame(384, $joined, 'anonymous visitors got the 384 names that are not kept back');

    // A user logs in. Their signed joins to new names must work, whatever the anonymous ones did.
    [$user, $userSocket] = $connect();
    $data = json_encode(['user_id' => '7', 'user_info' => ['name' => 'Ada']], JSON_THROW_ON_ERROR);
    pusher_send($user, 'pusher:subscribe', ['channel' => 'private-user.7', 'auth' => sign($userSocket, 'private-user.7')]);
    $f = pusher_read_when_ready($user, $openWorker);
    assertSame('pusher_internal:subscription_succeeded', $f['event'], "private-user.7 after $joined anonymous joins: " . json_encode($f));
    pusher_send($user, 'pusher:subscribe', ['channel' => 'presence-room.1', 'auth' => sign($userSocket, 'presence-room.1', $data), 'channel_data' => $data]);
    $f = pusher_read_when_ready($user, $openWorker);
    assertSame('pusher_internal:subscription_succeeded', $f['event'], 'presence-room.1: ' . json_encode($f));

    // A name somebody already holds stays joinable when the budget of new names is used up: it costs no topic.
    pusher_send($user, 'pusher:subscribe', ['channel' => 'flood.0.0']);
    $f = pusher_read_when_ready($user, $openWorker);
    assertSame('pusher_internal:subscription_succeeded', $f['event'], 'a name that is held: ' . json_encode($f));

    // A new public name: refused for another anonymous visitor (the budget that is not kept back is used up), admitted for
    // the signed-in user, whose signature on private-user.7 verified (ADR-0042 A4).
    [$late] = $connect();
    pusher_send($late, 'pusher:subscribe', ['channel' => 'news.late']);
    $f = pusher_read_when_ready($late, $openWorker);
    assertSame('pusher:subscription_error', $f['event'], 'a new name for an anonymous visitor: ' . json_encode($f));
    assertSame('LimitReached', $f['data']['type']);
    pusher_send($user, 'pusher:subscribe', ['channel' => 'news.user']);
    $f = pusher_read_when_ready($user, $openWorker);
    assertSame('pusher_internal:subscription_succeeded', $f['event'], 'a new name for a signed-in user: ' . json_encode($f));

    // What the operator sees: the refusals are the gateway's own, not the daemon's.
    $stats = $openGateway->stats();
    assertSame(0, $stats->daemonRefused, 'the daemon refused nothing: the gateway kept the anonymous joins inside its budget');
    assertSame($refused + 1, $stats->publicRefused);
    assertSame($joined + 1, $stats->publicChannels);

    fclose($late);
    fclose($user);
    foreach ($attackers as $ws) {
        fclose($ws);
    }
});

proc_terminate($open['process'], SIGTERM);
proc_terminate($small['process'], SIGTERM);
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
