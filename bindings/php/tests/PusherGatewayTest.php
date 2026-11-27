<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Pusher\Gateway;
use Wsrv\Pusher\GatewayConfig;
use Wsrv\Pusher\Link;
use Wsrv\MessageTooLarge;
use Wsrv\Pusher\Signature;
use Wsrv\StatusCode;

/** A Link that remembers every command and answers none; the tests play the daemon. */
final class FakeLink implements Link
{
    /** @var array<int, list<string>> */
    public array $sent = [];
    /** @var list<array{int, string, int}> conn, topic, request */
    public array $subscribed = [];
    /** @var list<array{int, string}> */
    public array $unsubscribed = [];
    /** @var list<array{string, string, int}> topic, payload, except */
    public array $published = [];
    /** @var list<array{int, int, string}> */
    public array $closed = [];
    /** @var list<array{string, int, string, int}> every subscribe and unsubscribe in the order the daemon would see them: kind, conn, topic, request */
    public array $log = [];
    /** The largest message the "daemon" takes; a bigger one is refused as Wsrv\Client refuses it. */
    public int $maxMessage = PHP_INT_MAX;
    /** The request id of the latest send(), so a test can answer it. */
    public int $lastSend = 0;
    private int $sends = 0;
    private int $next = 1;

    public function send(int $conn, string $text): int
    {
        if (strlen($text) > $this->maxMessage) {
            throw new MessageTooLarge('message is larger than the daemon accepts');
        }
        $this->sent[$conn][] = $text;
        // Their own range, so a test that counts subscribe ids from 1 is not disturbed by the sends between them.
        $this->lastSend = 1_000_000 + ++$this->sends;
        return $this->lastSend;
    }

    public function subscribe(int $conn, string $topic): int
    {
        $this->subscribed[] = [$conn, $topic, $this->next];
        $this->log[] = ['sub', $conn, $topic, $this->next];
        return $this->next++;
    }

    public function unsubscribe(int $conn, string $topic): int
    {
        $this->unsubscribed[] = [$conn, $topic];
        $this->log[] = ['unsub', $conn, $topic, $this->next];
        return $this->next++;
    }

    public function publish(string $topic, string $payload, int $except): void
    {
        $this->published[] = [$topic, $payload, $except];
    }

    public function close(int $conn, int $code, string $reason): void
    {
        $this->closed[] = [$conn, $code, $reason];
    }

    /** The request id of the latest subscribe. */
    public function lastRequest(): int
    {
        return $this->subscribed[array_key_last($this->subscribed)][2];
    }

    /** @return list<array> the frames sent to $conn, decoded: event, channel, data (decoded again if a string) */
    public function frames(int $conn): array
    {
        $out = [];
        foreach ($this->sent[$conn] ?? [] as $text) {
            $f = json_decode($text, true, 16, JSON_THROW_ON_ERROR);
            if (isset($f['data']) && is_string($f['data'])) {
                $f['data'] = json_decode($f['data'], true, 16, JSON_THROW_ON_ERROR);
            }
            $out[] = $f;
        }
        return $out;
    }

    public function lastFrame(int $conn): array
    {
        $frames = $this->frames($conn);
        return $frames[array_key_last($frames)] ?? [];
    }

    public function clear(): void
    {
        $this->sent = $this->published = $this->subscribed = $this->unsubscribed = $this->closed = $this->log = [];
    }
}

const GW_KEY = 'app-key-1234567890';
const GW_SECRET = 'a-long-enough-app-secret-0123456789';

/** @return array{0: Gateway, 1: FakeLink, 2: object} the clock is an object with a public float $now */
function gateway(array $overrides = []): array
{
    $link = new FakeLink();
    $clock = new class {
        public float $now = 1000.0;
    };
    $config = new GatewayConfig(GW_KEY, GW_SECRET, ...$overrides);
    return [new Gateway($config, $link, fn (): float => $clock->now), $link, $clock];
}

/** Open $conn and return its socket id. */
function open_conn(Gateway $gw, FakeLink $link, int $conn): string
{
    $gw->opened($conn);
    return $link->frames($conn)[0]['data']['socket_id'];
}

function frame_text(string $event, array $data, ?string $channel = null): string
{
    $f = ['event' => $event, 'data' => $data];
    if ($channel !== null) {
        $f['channel'] = $channel;
    }
    return json_encode($f, JSON_THROW_ON_ERROR);
}

/** Subscribe $conn to a channel the way pusher-js does, signing when the channel needs it. */
function subscribe_frame(string $channel, string $socketId, ?string $channelData = null, ?string $secret = GW_SECRET): string
{
    $data = ['channel' => $channel];
    if (str_starts_with($channel, 'private-') || str_starts_with($channel, 'presence-')) {
        $data['auth'] = Signature::authorize(GW_KEY, $secret, $socketId, $channel, $channelData);
    }
    if ($channelData !== null) {
        $data['channel_data'] = $channelData;
    }
    return frame_text('pusher:subscribe', $data);
}

/** Subscribe and let the daemon say OK. */
function join_channel(Gateway $gw, FakeLink $link, int $conn, string $socketId, string $channel, ?string $channelData = null): void
{
    $gw->received($conn, subscribe_frame($channel, $socketId, $channelData));
    $gw->status($link->lastRequest(), StatusCode::OK);
}

function user_data(string $id, ?array $info = null): string
{
    $d = ['user_id' => $id];
    if ($info !== null) {
        $d['user_info'] = $info;
    }
    return json_encode($d, JSON_THROW_ON_ERROR);
}

// ---- configuration --------------------------------------------------------------------------------

test('pusher config: a weak secret, an odd key or a nonsense bound is refused', function () {
    new GatewayConfig(GW_KEY, GW_SECRET);
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, 'short'));
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig('', GW_SECRET));
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig('bad key!', GW_SECRET));
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, GW_SECRET, maxFrameBytes: 0));
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, GW_SECRET, maxPresenceMembers: 0));
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, GW_SECRET, messagesPerSecond: 0));
    // a page must be able to resubscribe all its channels at once, so the burst cannot be smaller than the quota
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, GW_SECRET, maxChannelsPerConnection: 64, messageBurst: 70));
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, GW_SECRET, messageBurst: 0));
    new GatewayConfig(GW_KEY, GW_SECRET, maxChannelsPerConnection: 64, messageBurst: 80);
    assertThrows(InvalidArgumentException::class, fn () => new GatewayConfig(GW_KEY, GW_SECRET, activityTimeout: 5));
});

test('pusher config: the secret does not appear when the object is printed, dumped, encoded or in a stack trace', function () {
    $config = new GatewayConfig(GW_KEY, GW_SECRET);
    assertTrue(!str_contains(print_r($config, true), GW_SECRET), 'print_r');
    ob_start();
    var_dump($config);
    $dumped = (string) ob_get_clean();
    assertTrue(!str_contains($dumped, GW_SECRET), 'var_dump');
    assertTrue(!str_contains(json_encode($config) ?: '', GW_SECRET), 'json');
    assertThrows(LogicException::class, fn () => serialize($config), 'serialize');
    $weak = 'weak-secret';
    try {
        new GatewayConfig(GW_KEY, $weak);
        fail('a weak secret was accepted');
    } catch (InvalidArgumentException $e) {
        assertTrue(!str_contains($e->getMessage() . $e->getTraceAsString(), $weak), 'exception text and trace');
    }
});

test('pusher stats: every field is reported under its own name, in the documented order', function () {
    // Distinct values, so a field wired to the wrong key (or two swapped) cannot pass.
    $named = [
        'connections' => 1, 'channels' => 2, 'joinsInProgress' => 3, 'presenceChannels' => 4, 'presenceMembers' => 5,
        'memoryBytes' => 6, 'opened' => 7, 'frames' => 8, 'joins' => 9, 'badFrames' => 10, 'unauthorized' => 11,
        'rateLimited' => 12, 'subscriptionErrors' => 13, 'daemonRefused' => 14, 'strikeCloses' => 15,
    ];
    $stats = new \Wsrv\Pusher\GatewayStats(...$named);
    assertSame([
        'connections' => 1, 'channels' => 2, 'joins_in_progress' => 3, 'presence_channels' => 4, 'presence_members' => 5,
        'memory_bytes' => 6, 'opened' => 7, 'frames' => 8, 'joins' => 9, 'bad_frames' => 10, 'unauthorized' => 11,
        'rate_limited' => 12, 'subscription_errors' => 13, 'daemon_refused' => 14, 'strike_closes' => 15,
    ], $stats->toArray());
    assertSame('connections=1 channels=2 joins_in_progress=3 presence_channels=4 presence_members=5 memory_bytes=6 opened=7 frames=8 joins=9 bad_frames=10 unauthorized=11 rate_limited=12 subscription_errors=13 daemon_refused=14 strike_closes=15', $stats->line());
});

test('the app secret and the tokens do not appear when a ConnectionConfig or a Publisher is printed, dumped or serialized', function () {
    $secret = 'laravel-secret-0123456789abcdef';
    $token = 'wsrv1_' . str_repeat('ab', 32) . '_01234567';
    $config = \Wsrv\Laravel\ConnectionConfig::fromArray(laravel_config(['secret' => $secret, 'publish_token' => $token, 'ipc_token' => $token]));
    $publisher = new \Wsrv\Publisher('http://127.0.0.1:9200', $token);
    foreach ([$config, $publisher] as $object) {
        $name = $object::class;
        foreach ([$secret, $token] as $value) {
            assertTrue(!str_contains(print_r($object, true), $value), "$name print_r");
            ob_start();
            var_dump($object);
            assertTrue(!str_contains((string) ob_get_clean(), $value), "$name var_dump");
            assertTrue(!str_contains(json_encode($object) ?: '', $value), "$name json");
        }
        assertThrows(LogicException::class, fn () => serialize($object), "$name serialize");
    }
});

// ---- connecting -----------------------------------------------------------------------------------

test('pusher gateway: a new connection is told its socket id and the activity timeout', function () {
    [$gw, $link] = gateway();
    $gw->opened(41);
    $frame = $link->lastFrame(41);
    assertSame('pusher:connection_established', $frame['event']);
    assertSame(30, $frame['data']['activity_timeout']);
    assertTrue(preg_match('/\A41\.[0-9]+\z/', $frame['data']['socket_id']) === 1);
});

test('pusher gateway: ping is answered with pong and pong is ignored', function () {
    [$gw, $link] = gateway();
    open_conn($gw, $link, 1);
    $gw->received(1, '{"event":"pusher:ping"}');
    assertSame('pusher:pong', $link->lastFrame(1)['event']);
    $before = count($link->frames(1));
    $gw->received(1, '{"event":"pusher:pong"}');
    assertSame($before, count($link->frames(1)));
});

// ---- public channels ------------------------------------------------------------------------------

test('pusher gateway: a public channel is joined and only reported after the daemon says OK', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $sock));
    assertSame([[1, 'orders', 1]], $link->subscribed);
    assertSame(1, count($link->frames(1)), 'nothing yet beyond connection_established');
    $gw->status(1, StatusCode::OK);
    $f = $link->lastFrame(1);
    assertSame('pusher_internal:subscription_succeeded', $f['event']);
    assertSame('orders', $f['channel']);
    assertSame([], $f['data']);
});

test('pusher gateway: a join whose answer the daemon could not queue closes that browser, which reconnects and joins again', function () {
    // The daemon answers every send with a STATUS. BACKPRESSURE for the message that tells a browser it joined
    // means the page waits for ever while the room counts it as a member: closing it is the honest answer.
    [$gw, $link] = gateway();
    $a = open_conn($gw, $link, 1);
    $b = open_conn($gw, $link, 2);
    $gw->received(1, subscribe_frame('orders', $a));
    $gw->received(2, subscribe_frame('orders', $b));
    $gw->status(1, StatusCode::OK);
    $answerA = $link->lastSend;
    $gw->status(2, StatusCode::OK);
    $answerB = $link->lastSend;
    $gw->status($answerA, StatusCode::BACKPRESSURE);
    $gw->status($answerB, StatusCode::OK);
    assertSame([[1, 1011, 'could not answer a join']], $link->closed, 'only the browser that was not answered is closed');
    $stats = $gw->stats();
    assertSame(1, $stats->daemonRefused);
    $gw->closed(1);
    $gw->closed(2);
    assertSame(0, $gw->stats()->connections);
});

test('pusher gateway: a join answer to a browser that has gone, or one the daemon took, closes nothing', function () {
    [$gw, $link] = gateway();
    $a = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $a));
    $gw->status(1, StatusCode::OK);
    $answer = $link->lastSend;
    $gw->status($answer, StatusCode::NO_SUCH_CONN);   // the browser left first
    $gw->status($answer, StatusCode::BACKPRESSURE);   // an id is answered once: a second answer is nobody's
    assertSame([], $link->closed);
    assertSame(0, $gw->stats()->daemonRefused);
});

test('pusher gateway: a connection that closes before the daemon answers its join answer leaves nothing behind', function () {
    [$gw, $link] = gateway();
    $a = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $a));
    $gw->status(1, StatusCode::OK);
    $answer = $link->lastSend;
    $gw->closed(1);
    assertSame([], (new ReflectionProperty($gw, 'sendOwner'))->getValue($gw), 'forgotten when the connection closed, not when a status happens to come');
    $gw->status($answer, StatusCode::BACKPRESSURE);   // late: the connection is gone
    assertSame([], $link->closed);
});

test('pusher gateway: a refused subscription is reported as one and the channel is not joined', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $sock));
    $gw->status(1, StatusCode::LIMIT);
    $f = $link->lastFrame(1);
    assertSame('pusher:subscription_error', $f['event']);
    assertSame('LimitReached', $f['data']['type']);
    assertSame(4300, $f['data']['status']);
    // and it is not a member: an unsubscribe for it does nothing
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'orders']));
    assertSame([], $link->unsubscribed);
});

test('pusher gateway: stats count the joins the daemon refused (its broker limits), and not a browser that left first', function () {
    // This is how an operator sees that broker_max_topics (default 1024) or a sibling bound is too small for the
    // site: every refused join is a subscription_error to the browser, and daemon_refused says whose limit it was.
    [$gw, $link] = gateway();
    $a = open_conn($gw, $link, 1);
    foreach (['one', 'two', 'three', 'four'] as $name) {
        $gw->received(1, subscribe_frame($name, $a));
    }
    $requests = array_column($link->subscribed, 2);
    $gw->status($requests[0], StatusCode::LIMIT);              // a broker bound: refused
    $gw->status($requests[1], StatusCode::INTERNAL);           // any other error: refused too
    $gw->status($requests[2], StatusCode::NO_SUCH_CONN);       // the browser left first: not a refusal
    $gw->status($requests[3], StatusCode::OK);
    $stats = $gw->stats();
    assertSame(2, $stats->daemonRefused);
    assertSame(2, $stats->subscriptionErrors, 'each refusal was reported to its browser');
    assertSame(1, $stats->joins);
    // quota and name errors are the gateway's own, not the daemon's
    $gw->received(1, subscribe_frame('bad name', $a));
    assertSame(2, $gw->stats()->daemonRefused);
    assertSame(3, $gw->stats()->subscriptionErrors);
});

test('pusher gateway: joining the same channel twice asks the daemon once', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $sock, 'orders');
    $gw->received(1, subscribe_frame('orders', $sock));
    assertSame(1, count($link->subscribed));
    assertSame('pusher_internal:subscription_succeeded', $link->lastFrame(1)['event']);
    // while the first is still pending, the second is ignored
    $gw->received(1, subscribe_frame('news', $sock));
    $gw->received(1, subscribe_frame('news', $sock));
    assertSame(2, count($link->subscribed));
});

test('pusher gateway: a name wsrv cannot carry is refused before the daemon is asked', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    foreach (['a=b', 'a,b', 'a;b', str_repeat('a', 129), 'cache-x', 'private-cache-x', 'has space'] as $name) {
        $gw->received(1, subscribe_frame($name, $sock));
        $f = $link->lastFrame(1);
        assertSame('pusher:subscription_error', $f['event'], $name);
        assertSame('InvalidChannelName', $f['data']['type'], $name);
    }
    assertSame([], $link->subscribed);
});

test('pusher gateway: the channel quota of one connection is enforced, counting joins in progress', function () {
    [$gw, $link] = gateway(['maxChannelsPerConnection' => 3, 'messageBurst' => 20]);
    $sock = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $sock, 'a');
    join_channel($gw, $link, 1, $sock, 'b');
    $gw->received(1, subscribe_frame('c', $sock));        // pending, the third
    $gw->received(1, subscribe_frame('d', $sock));        // over quota with the pending one counted
    $f = $link->lastFrame(1);
    assertSame('LimitReached', $f['data']['type']);
    assertSame(3, count($link->subscribed));
});

test('pusher gateway: a page that joins every channel it may at once, with the default limits, gets them all', function () {
    // pusher-js sends all its subscribes back to back, on load and again after every reconnect, before
    // the daemon has answered any of them. Nothing in the defaults may turn that into an error.
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    $max = (new GatewayConfig(GW_KEY, GW_SECRET))->maxChannelsPerConnection;
    $requests = [];
    for ($i = 0; $i < $max; $i++) {
        $gw->received(1, subscribe_frame("bulk.$i", $sock));
        $requests[] = $link->lastRequest();
    }
    $gw->received(1, '{"event":"pusher:ping"}');
    $gw->received(1, '{"event":"pusher:ping"}');
    assertSame($max, count($link->subscribed), 'every join was passed to the daemon');
    foreach ($requests as $request) {
        $gw->status($request, StatusCode::OK);
    }
    $events = array_count_values(array_column($link->frames(1), 'event'));
    assertSame($max, $events['pusher_internal:subscription_succeeded'] ?? 0, 'every join succeeded');
    assertTrue(!isset($events['pusher:error']) && !isset($events['pusher:subscription_error']), 'no error: ' . json_encode($events));
    assertSame([], $link->closed, 'the connection is still open');
    assertSame($max, $gw->channelsOf(1));
    // one more is over the quota, as it should be
    $gw->received(1, subscribe_frame('one-too-many', $sock));
    assertSame('LimitReached', $link->lastFrame(1)['data']['type']);
});

// ---- private channels -----------------------------------------------------------------------------

test('pusher gateway: a private channel needs the signature of this connection', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('private-orders.1', $sock));
    assertSame(1, count($link->subscribed));
    $gw->status(1, StatusCode::OK);
    assertSame('pusher_internal:subscription_succeeded', $link->lastFrame(1)['event']);
});

test('pusher gateway: a private subscription without a valid signature is refused with 4009 and joins nothing', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    $other = open_conn($gw, $link, 2);
    $bad = [
        'wrong secret' => subscribe_frame('private-a', $sock, null, 'another-secret-another-secret'),
        'signed for another connection' => subscribe_frame('private-a', $other),
        'no auth' => frame_text('pusher:subscribe', ['channel' => 'private-a']),
        'auth not a string' => frame_text('pusher:subscribe', ['channel' => 'private-a', 'auth' => ['x']]),
        'auth empty' => frame_text('pusher:subscribe', ['channel' => 'private-a', 'auth' => '']),
        'signed for another channel' => frame_text('pusher:subscribe', [
            'channel' => 'private-a', 'auth' => Signature::authorize(GW_KEY, GW_SECRET, $sock, 'private-b'),
        ]),
    ];
    foreach ($bad as $why => $text) {
        $gw->received(1, $text);
        $f = $link->lastFrame(1);
        assertSame('pusher:error', $f['event'], $why);
        assertSame(4009, $f['data']['code'], $why);
    }
    assertSame([], $link->subscribed);
});

test('pusher gateway: a signature cannot be replayed on another connection', function () {
    [$gw, $link] = gateway();
    $a = open_conn($gw, $link, 1);
    open_conn($gw, $link, 2);
    $text = subscribe_frame('private-a', $a);   // signed for connection 1's socket id
    $gw->received(2, $text);
    assertSame(4009, $link->lastFrame(2)['data']['code']);
    assertSame([], $link->subscribed);
});

// ---- presence channels ----------------------------------------------------------------------------

test('pusher gateway: joining a presence channel returns the members, and the others hear of the new one', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $s2 = open_conn($gw, $link, 2);
    join_channel($gw, $link, 1, $s1, 'presence-room', user_data('7', ['name' => 'Ada']));
    $f = $link->lastFrame(1);
    assertSame('pusher_internal:subscription_succeeded', $f['event']);
    assertSame(['presence' => ['count' => 1, 'ids' => ['7'], 'hash' => ['7' => ['name' => 'Ada']]]], $f['data']);
    assertSame(0, count($link->published), 'nobody else is there to hear of the first member');

    $link->published = [];
    join_channel($gw, $link, 2, $s2, 'presence-room', user_data('8'));
    $f2 = $link->lastFrame(2);
    assertSame(2, $f2['data']['presence']['count']);
    assertSame(['7', '8'], $f2['data']['presence']['ids']);
    assertSame(1, count($link->published));
    [$topic, $payload, $except] = $link->published[0];
    assertSame('presence-room', $topic);
    assertSame(2, $except, 'the new member is not told about itself');
    $ev = json_decode($payload, true);
    assertSame('pusher_internal:member_added', $ev['event']);
    assertSame('presence-room', $ev['channel']);
    assertSame(['user_id' => '8'], json_decode($ev['data'], true));
});

test('pusher gateway: the member hash is an object even when the ids look like a list', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'presence-room', user_data('0', ['n' => 1]));
    $raw = json_decode($link->sent[1][1], true);
    assertTrue(str_contains($raw['data'], '"hash":{"0":'), 'hash is a JSON object: ' . $raw['data']);
    $s2 = open_conn($gw, $link, 2);
    join_channel($gw, $link, 2, $s2, 'presence-room', user_data('1'));
    $raw2 = json_decode($link->sent[2][1], true);
    assertTrue(str_contains($raw2['data'], '"hash":{"0":{"n":1},"1":{}}'), 'no-info member is {}: ' . $raw2['data']);
});

test('pusher gateway: a user with two connections is one member and leaves with the last one', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $s2 = open_conn($gw, $link, 2);
    join_channel($gw, $link, 1, $s1, 'presence-room', user_data('7'));
    $link->published = [];
    join_channel($gw, $link, 2, $s2, 'presence-room', user_data('7'));
    assertSame([], $link->published, 'the same user joining again is not news');
    assertSame(1, $link->lastFrame(2)['data']['presence']['count']);

    $gw->closed(1);
    assertSame([], $link->published, 'the user is still there through connection 2');
    $gw->closed(2);
    assertSame(1, count($link->published));
    $ev = json_decode($link->published[0][1], true);
    assertSame('pusher_internal:member_removed', $ev['event']);
    assertSame(['user_id' => '7'], json_decode($ev['data'], true));
});

test('pusher gateway: unsubscribing leaves the daemon room and announces the departure of the last connection', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'presence-room', user_data('7'));
    $link->published = [];
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'presence-room']));
    assertSame([[1, 'presence-room']], $link->unsubscribed);
    assertSame(1, count($link->published));
    assertSame('pusher_internal:member_removed', json_decode($link->published[0][1], true)['event']);
    // nothing is sent back for an unsubscribe, and doing it again is harmless
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'presence-room']));
    assertSame(1, count($link->unsubscribed));
});

test('pusher gateway: presence channel_data that was not signed, or is unusable, is refused', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $signed = user_data('7');
    $forged = user_data('1');   // a different user than the one the application signed
    $gw->received(1, frame_text('pusher:subscribe', [
        'channel' => 'presence-room', 'channel_data' => $forged,
        'auth' => Signature::authorize(GW_KEY, GW_SECRET, $s1, 'presence-room', $signed),
    ]));
    assertSame(4009, $link->lastFrame(1)['data']['code'], 'swapped user');

    $unusable = [
        'not json' => 'not json',
        'no user id' => '{"user_info":{"a":1}}',
        'user id empty' => '{"user_id":""}',
        'user id an array' => '{"user_id":["1"]}',
        'user id too long' => json_encode(['user_id' => str_repeat('x', 129)]),
        'user_info a string' => '{"user_id":"1","user_info":"x"}',
        'a list' => '["1"]',
        'too big' => json_encode(['user_id' => '1', 'user_info' => ['x' => str_repeat('y', 3000)]]),
    ];
    foreach ($unusable as $why => $data) {
        $gw->received(1, frame_text('pusher:subscribe', [
            'channel' => 'presence-room', 'channel_data' => $data,
            'auth' => Signature::authorize(GW_KEY, GW_SECRET, $s1, 'presence-room', $data),
        ]));
        $f = $link->lastFrame(1);
        assertSame('pusher:error', $f['event'], $why);
        assertSame(4200, $f['data']['code'], $why);
    }
    assertSame([], $link->subscribed);
});

test('pusher gateway: an integer user id is accepted and becomes a string', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $data = '{"user_id":42,"user_info":{"n":1}}';
    join_channel($gw, $link, 1, $s1, 'presence-room', $data);
    assertSame(['42'], $link->lastFrame(1)['data']['presence']['ids']);
});

test('pusher gateway: the member cap refuses a new user and never a second connection of a member', function () {
    [$gw, $link] = gateway(['maxPresenceMembers' => 2]);
    $s = [];
    foreach ([1, 2, 3, 4] as $c) {
        $s[$c] = open_conn($gw, $link, $c);
    }
    join_channel($gw, $link, 1, $s[1], 'presence-room', user_data('a'));
    join_channel($gw, $link, 2, $s[2], 'presence-room', user_data('b'));
    $gw->received(3, subscribe_frame('presence-room', $s[3], user_data('c')));
    assertSame('LimitReached', $link->lastFrame(3)['data']['type'], 'refused before asking the daemon');
    join_channel($gw, $link, 4, $s[4], 'presence-room', user_data('a'));
    assertSame(2, $link->lastFrame(4)['data']['presence']['count']);
});

test('pusher gateway: a race for the last seat is settled when the daemon answers', function () {
    [$gw, $link] = gateway(['maxPresenceMembers' => 1]);
    $s1 = open_conn($gw, $link, 1);
    $s2 = open_conn($gw, $link, 2);
    $gw->received(1, subscribe_frame('presence-room', $s1, user_data('a')));
    $r1 = $link->lastRequest();
    $gw->received(2, subscribe_frame('presence-room', $s2, user_data('b')));   // seat looks free: nobody is in yet
    $r2 = $link->lastRequest();
    $gw->status($r1, StatusCode::OK);
    $gw->status($r2, StatusCode::OK);
    assertSame('LimitReached', $link->lastFrame(2)['data']['type']);
    assertSame([[2, 'presence-room']], $link->unsubscribed, 'the loser leaves the daemon room again');
});

// ---- closing --------------------------------------------------------------------------------------

test('pusher gateway: a closed connection is forgotten, with the joins it had in progress', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $s1));
    $request = $link->lastRequest();
    $gw->closed(1);
    $gw->status($request, StatusCode::OK);                 // a late answer for a connection that is gone
    $gw->received(1, '{"event":"pusher:ping"}');           // and a frame that raced the close
    assertSame(1, count($link->frames(1)), 'nothing was sent to a closed connection');
    $gw->closed(1);                                        // closing twice is harmless
    $gw->closed(999);                                      // so is closing one that never opened
});

test('pusher gateway: a status for a request it does not know is ignored', function () {
    [$gw, $link] = gateway();
    $gw->status(12345, StatusCode::OK);
    $gw->status(0, StatusCode::LIMIT);
    assertSame([], $link->sent);
});

// ---- hostile input --------------------------------------------------------------------------------

test('pusher gateway: a bad frame is answered with 4200 and a strike, and too many strikes close the connection', function () {
    [$gw, $link] = gateway(['maxStrikes' => 3]);
    open_conn($gw, $link, 1);
    $gw->received(1, 'not json');
    assertSame(4200, $link->lastFrame(1)['data']['code']);
    $gw->received(1, '{"event":"pusher:nonsense"}');
    assertSame([], $link->closed);
    $gw->received(1, '[]');
    assertSame([[1, 1008, 'protocol violations']], $link->closed);
});

test('pusher gateway: a rare mistake is never fatal: one bad frame an hour for ten hours does not close the connection', function () {
    // Strikes leak (ADR-0042 A2). Before, the count only went up: a page open for a week that sent one
    // malformed frame a day was closed on the eighth day, however well it behaved in between.
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    for ($hour = 0; $hour < 10; $hour++) {
        $gw->received(1, 'not json');
        $clock->now += 3600.0;
    }
    assertSame([], $link->closed, 'ten mistakes in ten hours are not abuse');
    assertSame(10, $gw->stats()->badFrames, 'every one was still answered and counted');
});

test('pusher gateway: a strike is forgiven for each quiet minute before the next: a burst closes at max_strikes, a drip slower than a minute never', function () {
    // a burst: eight at the same instant (the interop run does this against a real daemon, a few ms apart)
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    for ($i = 0; $i < 7; $i++) {
        $gw->received(1, 'x');
        $clock->now += 0.002;
    }
    assertSame([], $link->closed);
    $gw->received(1, 'x');
    assertSame([[1, 1008, 'protocol violations']], $link->closed, 'the eighth strike of a burst closes');

    // one every 30 s: nothing is forgiven (less than a minute passes between two), so it ends like a burst, slowly
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    for ($i = 1; $i <= 7; $i++) {
        $gw->received(1, 'x');
        $clock->now += 30.0;
    }
    assertSame([], $link->closed, 'seven strikes 30 s apart');
    $gw->received(1, 'x');
    assertSame(1, count($link->closed), 'the eighth closes');

    // the edge: 59 s apart is still too fast, 60 s apart forgives one each time
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    for ($i = 1; $i <= 8; $i++) {
        $gw->received(1, 'x');
        $clock->now += 59.0;
    }
    assertSame(1, count($link->closed), 'one a minute minus a second: closed at the eighth');
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    for ($i = 0; $i < 500; $i++) {
        $gw->received(1, 'x');
        $clock->now += 60.0;
    }
    assertSame([], $link->closed, 'one a minute is forgiven as fast as it comes, however long it goes on');

    // quiet time forgives as many strikes as it has full minutes, and no credit is kept beyond zero
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    for ($i = 0; $i < 5; $i++) {
        $gw->received(1, 'x');                      // 5 strikes
    }
    $clock->now += 185.0;                           // three full minutes: three forgiven
    $gw->received(1, 'x');                          // 5 - 3 + 1 = 3
    for ($i = 0; $i < 4; $i++) {
        $gw->received(1, 'x');                      // 7
    }
    assertSame([], $link->closed, 'seven');
    $gw->received(1, 'x');
    assertSame(1, count($link->closed), 'the eighth closes: three of the first five were forgiven, no more');
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    $clock->now += 86400.0;                         // a day of good behaviour is not a credit of strikes
    for ($i = 0; $i < 8; $i++) {
        $gw->received(1, 'x');
    }
    assertSame(1, count($link->closed), 'a day of silence buys nothing for the eight that follow');
});

test('pusher gateway: a clock that steps back adds no strikes, and time moving on from there forgives them', function () {
    // The real clock is monotonic; this guards the arithmetic against one that is not (and a test double).
    [$gw, $link, $clock] = gateway(['maxStrikes' => 8]);
    open_conn($gw, $link, 1);
    $gw->received(1, 'x');
    $clock->now -= 6000.0;
    $gw->received(1, 'x');
    assertSame([], $link->closed, 'two strikes, not a hundred: going back in time is not a debt');
    for ($i = 0; $i < 5; $i++) {
        $gw->received(1, 'x');
    }
    assertSame([], $link->closed, 'seven in all');
    $gw->received(1, 'x');
    assertSame(1, count($link->closed), 'the eighth closes');
});

test('pusher gateway: a binary message counts as a bad frame', function () {
    [$gw, $link] = gateway();
    open_conn($gw, $link, 1);
    $gw->malformed(1);
    assertSame(1, $gw->stats()->frames, 'a binary message is a frame received');
    assertSame(1, $gw->stats()->badFrames);
    assertSame(4200, $link->lastFrame(1)['data']['code']);
    $gw->malformed(404);   // unknown connection: ignored
});

test('pusher gateway: a frame over the bound is refused', function () {
    [$gw, $link] = gateway(['maxFrameBytes' => 200]);
    $sock = open_conn($gw, $link, 1);
    $gw->received(1, '{"event":"pusher:subscribe","data":{"channel":"' . str_repeat('a', 300) . '"}}');
    assertSame(4200, $link->lastFrame(1)['data']['code']);
    assertSame([], $link->subscribed);
});

test('pusher gateway: events that are neither pusher: nor client- are ignored without a reply', function () {
    [$gw, $link] = gateway();
    open_conn($gw, $link, 1);
    $gw->received(1, '{"event":"pusher_internal:member_added","channel":"presence-a","data":{"user_id":"1"}}');
    $gw->received(1, '{"event":"some.app.event","channel":"orders","data":{}}');
    assertSame(1, count($link->frames(1)));
    assertSame([], $link->published);
});

test('pusher gateway: messages are limited by a bucket: a burst is allowed, the rate refills it, it never holds more than the burst', function () {
    [$gw, $link, $clock] = gateway(['messagesPerSecond' => 3, 'messageBurst' => 17, 'maxChannelsPerConnection' => 1, 'maxStrikes' => 1000]);
    open_conn($gw, $link, 1);
    $ping = function () use ($gw, $link): string {
        $gw->received(1, '{"event":"pusher:ping"}');
        return $link->lastFrame(1)['event'];
    };
    for ($i = 0; $i < 17; $i++) {
        assertSame('pusher:pong', $ping(), "burst message $i");
    }
    $gw->received(1, '{"event":"pusher:ping"}');
    $f = $link->lastFrame(1);
    assertSame(4301, $f['data']['code']);
    assertSame('Rate limit exceeded', $f['data']['message']);
    $clock->now += 1.0;                                   // 3 tokens come back
    for ($i = 0; $i < 3; $i++) {
        assertSame('pusher:pong', $ping(), "refilled message $i");
    }
    assertSame('pusher:error', $ping(), 'the fourth is over the rate again');
    $clock->now += 3600.0;                                // an hour of silence is not an hour of credit
    for ($i = 0; $i < 17; $i++) {
        assertSame('pusher:pong', $ping(), "after the pause $i");
    }
    assertSame('pusher:error', $ping(), 'only the burst, not more');
});

test('pusher gateway: a flood that keeps going past the burst is closed, a client that stays within the rate never is', function () {
    [$gw, $link, $clock] = gateway(['messagesPerSecond' => 3, 'messageBurst' => 17, 'maxChannelsPerConnection' => 1, 'maxStrikes' => 4]);
    open_conn($gw, $link, 1);
    open_conn($gw, $link, 2);
    for ($i = 0; $i < 17; $i++) {
        $gw->received(1, '{"event":"pusher:ping"}');
    }
    assertSame([], $link->closed, 'the burst itself costs nothing');
    for ($i = 0; $i < 4; $i++) {
        $gw->received(1, '{"event":"pusher:ping"}');
    }
    assertSame([[1, 1008, 'protocol violations']], $link->closed, 'four refused messages are four strikes');
    for ($i = 0; $i < 100; $i++) {                         // 3 per second for a while: always within the rate
        $clock->now += 1.0;
        $gw->received(2, '{"event":"pusher:ping"}');
        $gw->received(2, '{"event":"pusher:ping"}');
        $gw->received(2, '{"event":"pusher:ping"}');
    }
    assertSame(1, count($link->closed));
});

test('pusher gateway: a clock that steps back gives a connection no credit, and time moving on from there does', function () {
    // The real clock is monotonic; this is the guard for one that is not (and for a test double).
    [$gw, $link, $clock] = gateway(['messagesPerSecond' => 3, 'messageBurst' => 17, 'maxChannelsPerConnection' => 1, 'maxStrikes' => 1000]);
    open_conn($gw, $link, 1);
    for ($i = 0; $i < 17; $i++) {
        $gw->received(1, '{"event":"pusher:ping"}');
    }
    $clock->now -= 600.0;
    $gw->received(1, '{"event":"pusher:ping"}');
    assertSame('pusher:error', $link->lastFrame(1)['event'], 'going back in time is not a refill');
    $clock->now += 1.0;
    for ($i = 0; $i < 3; $i++) {
        $gw->received(1, '{"event":"pusher:ping"}');
        assertSame('pusher:pong', $link->lastFrame(1)['event'], "refilled message $i");
    }
    $gw->received(1, '{"event":"pusher:ping"}');
    assertSame('pusher:error', $link->lastFrame(1)['event'], 'one second is three messages, no more');
});

test('pusher gateway: nothing the gateway sends or throws contains the app secret', function () {
    [$gw, $link] = gateway();
    $sock = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('private-a', $sock, null, 'another-secret-another-secret'));
    $gw->received(1, 'garbage');
    join_channel($gw, $link, 1, $sock, 'presence-r', user_data('1'));
    $all = implode("\n", $link->sent[1]) . implode("\n", array_column($link->published, 1));
    assertTrue(!str_contains($all, GW_SECRET), 'secret in output');
    assertTrue(!str_contains($all, Signature::authorize(GW_KEY, GW_SECRET, $sock, 'presence-r', user_data('1'))), 'signature echoed');
});

test('pusher gateway: random and hostile frames never throw and never break a bound', function () {
    [$gw, $link, $clock] = gateway(['maxChannelsPerConnection' => 4, 'maxStrikes' => 1_000_000, 'messagesPerSecond' => 1_000_000, 'messageBurst' => 1_000_000]);
    $sock = open_conn($gw, $link, 1);
    mt_srand(7);
    $events = ['pusher:subscribe', 'pusher:unsubscribe', 'pusher:ping', 'client-x', 'pusher:bogus', 'x', 5, null, ['a'], ''];
    $channels = ['a', 'private-a', 'presence-a', 'a=b', '', null, 7, [], str_repeat('z', 200), 'cache-a', "a\x00"];
    for ($i = 0; $i < 3000; $i++) {
        $ev = $events[mt_rand(0, count($events) - 1)];
        $ch = $channels[mt_rand(0, count($channels) - 1)];
        $data = match (mt_rand(0, 5)) {
            0 => null,
            1 => ['channel' => $ch],
            2 => ['channel' => $ch, 'auth' => Signature::authorize(GW_KEY, GW_SECRET, $sock, is_string($ch) ? $ch : 'x')],
            3 => json_encode(['channel' => $ch, 'channel_data' => mt_rand(0, 1) ? '{"user_id":"1"}' : 5]),
            4 => 'text',
            default => [[[[[[[[[1]]]]]]]]],
        };
        $frame = ['event' => $ev, 'channel' => $ch, 'data' => $data];
        if (mt_rand(0, 9) === 0) {
            unset($frame['event']);
        }
        $text = json_encode($frame);
        if ($text === false) {
            continue;
        }
        $gw->received(1, mt_rand(0, 19) === 0 ? substr($text, 0, mt_rand(0, strlen($text))) : $text);
        if ($link->subscribed !== [] && mt_rand(0, 2) === 0) {
            $gw->status($link->lastRequest(), mt_rand(0, 4) === 0 ? StatusCode::LIMIT : StatusCode::OK);
        }
    }
    assertTrue($gw->channelsOf(1) <= 4, 'channel quota held: ' . $gw->channelsOf(1));
    assertTrue($gw->pendingCount() <= 4, 'joins in progress are bounded by the quota: ' . $gw->pendingCount());
    $gw->closed(1);
    assertSame(0, $gw->pendingCount(), 'no join survives its connection');
    assertSame(0, $gw->presenceChannels(), 'no member survives its connection');
});

// ---- asking to join a channel one is already in -------------------------------------------------------
// Pusher answers a repeated subscribe with the list again (Reverb does too), and pusher-js never sends one,
// but any other client may send them as fast as the message rate allows. The list costs the worker time in
// proportion to the members, so a repeat is answered from the frame built the last time while the members
// are the same ones, and from a new one as soon as they are not.

/** @return list<string> the member ids in the latest answer to $conn, after it asked to join $channel again */
function repeat_ids(Gateway $gw, FakeLink $link, int $conn, string $socketId, string $channel, string $channelData): array
{
    $link->clear();
    $gw->received($conn, subscribe_frame($channel, $socketId, $channelData));
    $f = $link->lastFrame($conn);
    assertSame('pusher_internal:subscription_succeeded', $f['event']);
    return $f['data']['presence']['ids'];
}

test('pusher gateway: a repeated presence subscribe always lists the members who are in now', function () {
    [$gw, $link] = gateway();
    $s = [];
    for ($i = 1; $i <= 4; $i++) {
        $s[$i] = open_conn($gw, $link, $i);
    }
    $room = 'presence-room';
    join_channel($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada']));
    assertSame(['1'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])));
    assertSame(['1'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])), 'asked twice, answered twice');
    join_channel($gw, $link, 2, $s[2], $room, user_data('2', ['name' => 'Bob']));
    assertSame(['1', '2'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])), 'a new member is listed');
    join_channel($gw, $link, 3, $s[3], $room, user_data('2', ['name' => 'Bob in another tab']));
    assertSame(['1', '2'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])), 'a second tab is no new member');
    $gw->closed(2);
    assertSame(['1', '2'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])), 'Bob is still in through the other tab');
    $gw->closed(3);
    assertSame(['1'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])), 'Bob left with his last tab');
    join_channel($gw, $link, 4, $s[4], $room, user_data('3', ['name' => 'Cy']));
    assertSame(['1', '3'], repeat_ids($gw, $link, 1, $s[1], $room, user_data('1', ['name' => 'Ada'])));
    // the room emptied and made again is a different room, whatever was kept for the old one
    $gw->closed(4);
    $gw->closed(1);
    $s[5] = open_conn($gw, $link, 5);
    join_channel($gw, $link, 5, $s[5], $room, user_data('9', ['name' => 'Di']));
    assertSame(['9'], repeat_ids($gw, $link, 5, $s[5], $room, user_data('9', ['name' => 'Di'])));
});

test('pusher gateway: a repeated presence subscribe reuses the list while the members are the same, and builds it again when they change', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $s2 = open_conn($gw, $link, 2);
    $room = 'presence-room';
    join_channel($gw, $link, 1, $s1, $room, user_data('1', ['name' => 'Ada']));
    $kept = new ReflectionProperty($gw, 'listFrames');
    assertSame([], $kept->getValue($gw), 'the join itself keeps nothing: the next join would change it at once');

    $link->clear();
    $gw->received(1, subscribe_frame($room, $s1, user_data('1', ['name' => 'Ada'])));
    $first = $link->sent[1][0];
    $store = $kept->getValue($gw);
    assertSame([$room], array_keys($store), 'the first repeat builds the list and keeps it');
    assertSame($first, $store[$room][1]);

    // Put a marker where the kept frame is: while nothing changed, a repeat must come back with the marker.
    $store[$room][1] = 'KEPT FRAME';
    $kept->setValue($gw, $store);
    $link->clear();
    $gw->received(1, subscribe_frame($room, $s1, user_data('1', ['name' => 'Ada'])));
    assertSame(['KEPT FRAME'], $link->sent[1], 'the same members: the answer is the kept frame');

    // A member comes: the kept frame is out of date, and the repeat must say who is in now.
    join_channel($gw, $link, 2, $s2, $room, user_data('2', ['name' => 'Bob']));
    $link->clear();
    $gw->received(1, subscribe_frame($room, $s1, user_data('1', ['name' => 'Ada'])));
    assertSame(['1', '2'], $link->lastFrame(1)['data']['presence']['ids']);
    assertTrue($link->sent[1][0] !== 'KEPT FRAME');
    // and that new list is what is kept now
    $store = $kept->getValue($gw);
    assertSame($link->sent[1][0], $store[$room][1]);
});

test('pusher gateway: the lists kept for repeated subscribes are bounded, oldest first out', function () {
    [$gw, $link] = gateway(['maxChannelsPerConnection' => 40]);
    $s = open_conn($gw, $link, 1);
    for ($i = 0; $i < 20; $i++) {
        $room = "presence-room-$i";
        join_channel($gw, $link, 1, $s, $room, user_data('1', ['n' => $i]));
        $gw->received(1, subscribe_frame($room, $s, user_data('1', ['n' => $i])));   // a repeat keeps the list
    }
    $keys = array_keys((new ReflectionProperty($gw, 'listFrames'))->getValue($gw));
    assertSame(16, count($keys), 'no more than 16 lists are kept');
    assertSame('presence-room-4', $keys[0], 'the oldest four went first');
    assertSame('presence-room-19', $keys[15]);
    // A list that is built again (its room changed) counts as the newest: the next one in pushes out the oldest, not it.
    $s2 = open_conn($gw, $link, 2);
    join_channel($gw, $link, 2, $s2, 'presence-room-4', user_data('2', ['n' => 'a new member']));
    $gw->received(1, subscribe_frame('presence-room-4', $s, user_data('1', ['n' => 4])));
    join_channel($gw, $link, 1, $s, 'presence-room-20', user_data('1', ['n' => 20]));
    $gw->received(1, subscribe_frame('presence-room-20', $s, user_data('1', ['n' => 20])));
    $keys = array_keys((new ReflectionProperty($gw, 'listFrames'))->getValue($gw));
    assertSame(16, count($keys));
    assertSame('presence-room-6', $keys[0], 'room 4 was refreshed, so room 5 was the oldest and went');
    assertSame(['presence-room-4', 'presence-room-20'], array_slice($keys, -2));
});

test('pusher gateway: a stale kept list cannot be sent even if a change to the members was missed', function () {
    // The gateway drops a room's kept list whenever it changes the members, and the table's revision is the
    // second guard: change the table behind the gateway's back (as a future code path that forgot to drop
    // would) and a repeat must still say who is in, and the rebuilt list must count as the newest.
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $ada = user_data('1', ['name' => 'Ada']);
    foreach (['presence-a', 'presence-b'] as $room) {
        join_channel($gw, $link, 1, $s1, $room, $ada);
        $gw->received(1, subscribe_frame($room, $s1, $ada));
    }
    assertSame(['presence-a', 'presence-b'], array_keys(kept_lists($gw)));
    (new ReflectionProperty($gw, 'presence'))->getValue($gw)->add('presence-a', 99, 'ghost', null);
    $link->clear();
    $gw->received(1, subscribe_frame('presence-a', $s1, $ada));
    assertSame(['1', 'ghost'], $link->lastFrame(1)['data']['presence']['ids'], 'the kept list was out of date and was not used');
    assertSame(['presence-b', 'presence-a'], array_keys(kept_lists($gw)), 'the rebuilt list is the newest');
});

/** @return array<string, array{int, string}> what the gateway keeps for repeated subscribes, by channel */
function kept_lists(Gateway $gw): array
{
    return (new ReflectionProperty($gw, 'listFrames'))->getValue($gw);
}

test('pusher gateway: a kept list is dropped when the members change, so it never holds who has left', function () {
    [$gw, $link] = gateway();
    $s = [];
    for ($i = 1; $i <= 3; $i++) {
        $s[$i] = open_conn($gw, $link, $i);
    }
    $room = 'presence-room';
    $ada = user_data('1', ['name' => 'Ada']);
    join_channel($gw, $link, 1, $s[1], $room, $ada);
    $gw->received(1, subscribe_frame($room, $s[1], $ada));
    assertSame([$room], array_keys(kept_lists($gw)), 'a repeat keeps the list');
    // a second tab of Ada is no change: what was kept is still right, and stays
    join_channel($gw, $link, 3, $s[3], $room, user_data('1', ['name' => 'Ada, in another tab']));
    assertSame([$room], array_keys(kept_lists($gw)));
    $gw->closed(3);
    assertSame([$room], array_keys(kept_lists($gw)), 'one of two tabs closing is no change either');
    // a new member: the kept list is out of date, and goes at once instead of waiting to be found out
    join_channel($gw, $link, 2, $s[2], $room, user_data('2', ['name' => 'Bob']));
    assertSame([], kept_lists($gw), 'Bob came');
    $gw->received(1, subscribe_frame($room, $s[1], $ada));
    assertSame([$room], array_keys(kept_lists($gw)));
    $gw->closed(2);
    assertSame([], kept_lists($gw), 'Bob left: the list that names him does not stay in memory');
    $gw->received(1, subscribe_frame($room, $s[1], $ada));
    assertSame([$room], array_keys(kept_lists($gw)));
    $gw->closed(1);
    assertSame([], kept_lists($gw), 'the room is empty: nothing is kept for it');
});

test('pusher gateway: a list is kept up to a size, and a larger one is answered each time but not kept', function () {
    // The lists of the default limits (100 members of 2 KiB) are about 400 KB at most. A site that raises
    // max_channel_data_bytes and max_presence_members can make them far larger, and sixteen of those must
    // not sit in the worker's memory for good: above 1 MiB a list is built for each repeat, as before.
    [$gw, $link] = gateway(['maxChannelDataBytes' => 65536, 'maxFrameBytes' => 1 << 20]);
    $room = 'presence-big';
    $join = function (int $first, int $last, int $bytes) use ($gw, $link, $room): void {
        for ($i = $first; $i <= $last; $i++) {
            $s = open_conn($gw, $link, $i);
            join_channel($gw, $link, $i, $s, $room, user_data((string) $i, ['bio' => str_repeat('x', $bytes)]));
        }
    };
    $join(1, 20, 20000);                                  // about 400 KB: what the defaults can come to
    $s1 = $link->frames(1)[0]['data']['socket_id'];
    $again = fn (): array => (function () use ($gw, $link, $room, $s1): array {
        $link->clear();
        $gw->received(1, subscribe_frame($room, $s1, user_data('1', ['bio' => str_repeat('x', 20000)])));
        return $link->lastFrame(1)['data']['presence']['ids'];
    })();
    assertSame(20, count($again()));
    $kept = kept_lists($gw);
    assertSame([$room], array_keys($kept), 'about 400 KB is kept');
    assertTrue(strlen($kept[$room][1]) > 380000 && strlen($kept[$room][1]) < 1 << 20);
    $join(21, 60, 20000);                                 // about 1.2 MB in all
    assertSame(60, count($again()), 'the larger list is answered, whole');
    assertSame([], kept_lists($gw), 'and not kept: it is over 1 MiB');
    assertSame(60, count($again()), 'asked again, it is the same list, built again');
});

test('pusher gateway: a presence member list that does not fit the daemon refuses that join and nothing else', function () {
    // The daemon's IPC frame is max_message_size + 64 KiB. A list of 100 members with 2 KiB each is 200 KiB:
    // with a small max_message_size the send throws, and that must fail one join, not reset the worker's link.
    [$gw, $link] = gateway();
    $info = ['bio' => str_repeat('b', 300)];
    $sockets = [];
    for ($i = 1; $i <= 7; $i++) {
        $sockets[$i] = open_conn($gw, $link, $i);
    }
    for ($i = 1; $i <= 5; $i++) {
        join_channel($gw, $link, $i, $sockets[$i], 'presence-big', user_data((string) $i, $info));
    }
    $link->clear();
    $before = $gw->stats();
    $link->maxMessage = 1000;                              // the sixth member's list is about 1.9 KB
    $gw->received(6, subscribe_frame('presence-big', $sockets[6], user_data('6', $info)));
    $gw->status($link->lastRequest(), StatusCode::OK);     // no exception: the join fails, the worker goes on
    $f = $link->lastFrame(6);
    assertSame('pusher:subscription_error', $f['event']);
    assertSame('ServerError', $f['data']['type']);
    assertSame([[6, 'presence-big']], $link->unsubscribed, 'the daemon room was left again');
    assertSame([], $link->published, 'nobody heard of a member who never joined');
    assertSame(0, $gw->channelsOf(6));
    assertSame(5, $gw->stats()->presenceMembers, 'the member table has no ghost');
    assertSame($before->joins, $gw->stats()->joins, 'a join that failed is not counted as a join');
    assertSame($before->subscriptionErrors + 1, $gw->stats()->subscriptionErrors);
    // the limit goes away (or the info shrinks): the same user can join, and the others are told then
    $link->maxMessage = PHP_INT_MAX;
    $link->clear();
    $gw->received(6, subscribe_frame('presence-big', $sockets[6], user_data('6', $info)));
    $gw->status($link->lastRequest(), StatusCode::OK);
    assertSame('pusher_internal:subscription_succeeded', $link->lastFrame(6)['event']);
    assertSame(6, $gw->stats()->presenceMembers);
    assertSame(1, count($link->published), 'the others are told once');
    // a second tab of a member that is already in needs the list too, and fails the same clean way
    $link->maxMessage = 1000;
    $link->clear();
    $gw->received(7, subscribe_frame('presence-big', $sockets[7], user_data('6', $info)));
    $gw->status($link->lastRequest(), StatusCode::OK);
    assertSame('pusher:subscription_error', $link->lastFrame(7)['event']);
    assertSame(6, $gw->stats()->presenceMembers, 'the user stays a member through the tab that is in');
    $gw->closed(7);
    $gw->closed(6);
    assertSame(5, $gw->stats()->presenceMembers);
});

test('pusher gateway: a repeated subscribe that no longer fits one frame takes the connection out of the channel', function () {
    // Joined while the list fitted; members came; the same connection subscribes again and the answer would
    // not fit. It cannot be told who is here, so it leaves like a connection that asked to, and the others
    // hear that its user is gone. (A repeated subscribe on a list that fits is answered again and changes nothing.)
    [$gw, $link] = gateway();
    $info = ['bio' => str_repeat('b', 300)];
    $sockets = [];
    for ($i = 1; $i <= 6; $i++) {
        $sockets[$i] = open_conn($gw, $link, $i);
        join_channel($gw, $link, $i, $sockets[$i], 'presence-big', user_data((string) $i, $info));
    }
    $link->clear();
    $gw->received(2, subscribe_frame('presence-big', $sockets[2], user_data('2', $info)));
    assertSame('pusher_internal:subscription_succeeded', $link->lastFrame(2)['event'], 'it still fits: answered again');
    assertSame(6, $gw->stats()->presenceMembers);
    $link->maxMessage = 1000;
    $link->clear();
    $gw->received(2, subscribe_frame('presence-big', $sockets[2], user_data('2', $info)));
    assertSame('pusher:subscription_error', $link->lastFrame(2)['event']);
    assertSame([[2, 'presence-big']], $link->unsubscribed, 'out of the daemon room');
    assertSame(0, $gw->channelsOf(2));
    assertSame(5, $gw->stats()->presenceMembers);
    assertSame(1, count($link->published), 'the others are told that user 2 is gone');
    assertSame('pusher_internal:member_removed', json_decode($link->published[0][1], true)['event']);
    $gw->closed(2);                                            // closing later finds nothing left to undo
    assertSame(5, $gw->stats()->presenceMembers);
});

// ---- statistics and churn (ADR-0042 A2) -----------------------------------------------------------------

test('pusher gateway: stats show what the gateway holds and count what happened', function () {
    [$gw, $link] = gateway(['maxStrikes' => 3, 'maxChannelsPerConnection' => 4]);
    $zero = $gw->stats();
    foreach (['connections', 'channels', 'joins_in_progress', 'presence_channels', 'presence_members', 'opened', 'frames',
        'joins', 'bad_frames', 'unauthorized', 'rate_limited', 'subscription_errors', 'daemon_refused', 'strike_closes'] as $key) {
        assertSame(0, $zero->toArray()[$key], "$key starts at zero");
    }
    assertTrue($zero->memoryBytes > 0, 'the memory in use is read from the process');

    $a = open_conn($gw, $link, 1);
    $b = open_conn($gw, $link, 2);
    join_channel($gw, $link, 1, $a, 'news');
    join_channel($gw, $link, 1, $a, 'private-x');
    join_channel($gw, $link, 1, $a, 'presence-r', user_data('7'));
    join_channel($gw, $link, 2, $b, 'presence-r', user_data('8'));
    $gw->received(2, subscribe_frame('private-y', $b));           // the daemon has not answered this one
    $s = $gw->stats();
    assertSame([2, 4, 1, 1, 2], [$s->connections, $s->channels, $s->joinsInProgress, $s->presenceChannels, $s->presenceMembers]);
    assertSame([2, 5, 4], [$s->opened, $s->frames, $s->joins]);

    $gw->received(1, 'not json');                                                       // 4200
    $gw->received(1, subscribe_frame('private-z', $a, null, 'another-secret-another-secret'));   // 4009
    $gw->received(1, subscribe_frame('a=b', $a));                                       // 4300 InvalidChannelName
    $gw->received(1, whisper('news', 'client-x', ['a' => 1]));                          // 4009: not a member of a private channel
    $s = $gw->stats();
    assertSame([1, 2, 1, 9], [$s->badFrames, $s->unauthorized, $s->subscriptionErrors, $s->frames]);
    assertSame(0, $s->strikeCloses);
    $gw->received(1, 'x');
    $gw->received(1, 'x');
    assertSame(1, $gw->stats()->strikeCloses, 'three strikes: the connection was closed, once');
    $gw->received(1, 'x');
    assertSame(1, $gw->stats()->strikeCloses, 'a connection that is closing is not closed again');

    $gw->closed(1);
    $s = $gw->stats();
    assertSame([1, 1, 1, 1, 1], [$s->connections, $s->channels, $s->joinsInProgress, $s->presenceChannels, $s->presenceMembers]);
    $gw->closed(2);
    $s = $gw->stats();
    assertSame([0, 0, 0, 0, 0], [$s->connections, $s->channels, $s->joinsInProgress, $s->presenceChannels, $s->presenceMembers], 'every gauge is back to zero');
    assertSame(2, $s->opened, 'counters never go down');
});

test('pusher gateway: stats count the frames the limiters refused', function () {
    [$gw, $link, $clock] = gateway(['messagesPerSecond' => 3, 'messageBurst' => 17, 'maxChannelsPerConnection' => 1, 'maxStrikes' => 1000, 'clientEventsPerSecond' => 2]);
    $sock = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $sock, 'private-w');
    for ($i = 0; $i < 18; $i++) {                  // one subscribe and 16 pings use the burst of 17; the next two are refused
        $gw->received(1, '{"event":"pusher:ping"}');
    }
    assertSame(2, $gw->stats()->rateLimited, 'two frames over the bucket');
    $clock->now += 10.0;
    for ($i = 0; $i < 3; $i++) {                   // 2 per second are allowed, the third is over
        $gw->received(1, whisper('private-w', 'client-t', ['n' => $i]));
    }
    assertSame(3, $gw->stats()->rateLimited, 'a whisper over its rate is counted too');
    assertSame(0, $gw->stats()->strikeCloses);
});

test('pusher gateway: stats line is one line of key=value numbers in a fixed order, and carries nothing a peer sent', function () {
    [$gw, $link] = gateway();
    open_conn($gw, $link, 1);
    $gw->received(1, '{"event":"pusher:subscribe","data":{"channel":"evil\nchannel=x connections=999"}}');
    $line = $gw->stats()->line();
    assertTrue(preg_match('/\A[a-z_]+=\d+( [a-z_]+=\d+)*\z/', $line) === 1, 'not a plain line: ' . $line);
    assertSame(
        ['connections', 'channels', 'joins_in_progress', 'presence_channels', 'presence_members', 'memory_bytes',
            'opened', 'frames', 'joins', 'bad_frames', 'unauthorized', 'rate_limited', 'subscription_errors', 'daemon_refused',
            'strike_closes'],
        array_keys($gw->stats()->toArray()),
    );
    assertSame(1, $gw->stats()->connections, 'and the number in the line is the real one');
    assertTrue(str_starts_with($line, 'connections=1 '), $line);
});

test('pusher gateway: churn: in any order of joins, answers, leaves and closes nothing is left behind and every bound holds', function () {
    $quota = 5;
    $members = 3;
    foreach ([1, 2, 3, 4, 5, 6, 7, 8] as $seed) {
        mt_srand($seed);
        [$gw, $link, $clock] = gateway([
            'maxChannelsPerConnection' => $quota, 'maxPresenceMembers' => $members, 'maxStrikes' => 1_000_000,
            'messagesPerSecond' => 1_000_000, 'messageBurst' => 1_000_000, 'clientEventsPerSecond' => 100_000,
        ]);
        $open = [];        // conn => socket id
        $rooms = [];       // the daemon's view: conn => [topic => true]
        $outstanding = []; // subscribe requests not answered yet: request => [conn, topic]
        $seen = 0;         // how much of $link->log has been applied to $rooms
        $next = 1;
        $topics = ['news', 'private-a', 'private-b', 'presence-r1', 'presence-r2', 'presence-r3'];
        $sync = function () use ($link, &$seen, &$rooms, &$outstanding, &$open): void {
            for (; $seen < count($link->log); $seen++) {
                [$kind, $conn, $topic, $request] = $link->log[$seen];
                if ($kind === 'sub') {
                    $outstanding[$request] = [$conn, $topic];
                    if (isset($open[$conn])) {
                        $rooms[$conn][$topic] = true;     // the daemon puts the connection in the room when it gets the command
                    }
                } elseif (isset($rooms[$conn])) {
                    unset($rooms[$conn][$topic]);
                }
            }
        };
        $answer = function (int $request, int $status) use ($gw, &$rooms, &$outstanding): void {
            [$conn, $topic] = $outstanding[$request];
            unset($outstanding[$request]);
            if ($status !== StatusCode::OK) {
                unset($rooms[$conn][$topic]);             // a refused subscribe leaves no room behind
            }
            $gw->status($request, $status);
        };
        for ($i = 0; $i < 3000; $i++) {
            $op = mt_rand(0, 99);
            $conn = $open === [] ? 0 : array_keys($open)[mt_rand(0, count($open) - 1)];
            if ($op < 8 && count($open) < 12) {
                $open[$next] = open_conn($gw, $link, $next);
                $next++;
            } elseif ($op < 13 && $conn !== 0) {
                $gw->closed($conn);
                unset($open[$conn], $rooms[$conn]);
            } elseif ($op < 45 && $conn !== 0) {
                $topic = $topics[mt_rand(0, count($topics) - 1)];
                $data = str_starts_with($topic, 'presence-') ? user_data((string) mt_rand(1, 5)) : null;
                $secret = mt_rand(0, 9) === 0 ? 'another-secret-another-secret' : GW_SECRET;
                $gw->received($conn, subscribe_frame($topic, $open[$conn], $data, $secret));
            } elseif ($op < 58 && $conn !== 0) {
                $gw->received($conn, frame_text('pusher:unsubscribe', ['channel' => $topics[mt_rand(0, count($topics) - 1)]]));
            } elseif ($op < 86 && $outstanding !== []) {
                $sync();
                $request = array_keys($outstanding)[mt_rand(0, count($outstanding) - 1)];
                $answer($request, [StatusCode::OK, StatusCode::OK, StatusCode::OK, StatusCode::LIMIT, StatusCode::NO_SUCH_CONN, StatusCode::INTERNAL][mt_rand(0, 5)]);
            } elseif ($op < 93 && $conn !== 0) {
                $gw->received($conn, whisper($topics[mt_rand(0, count($topics) - 1)], 'client-x', ['i' => $i]));
            } elseif ($conn !== 0) {
                $gw->received($conn, mt_rand(0, 1) === 0 ? 'garbage' : '{"event":"pusher:ping"}');
            }
            $clock->now += mt_rand(0, 3) === 0 ? 1.0 : 0.0;
            $sync();
            $s = $gw->stats();
            assertSame(count($open), $s->connections, "seed $seed step $i");
            assertTrue($s->channels <= count($open) * $quota, "seed $seed step $i: more memberships than the quota allows");
            assertTrue($s->joinsInProgress <= count($open) * $quota, "seed $seed step $i: more joins in progress than the quota allows");
            assertTrue($s->presenceMembers <= $s->presenceChannels * $members, "seed $seed step $i: a presence channel over its member cap");
            assertTrue($s->presenceChannels <= count($open) * $quota, "seed $seed step $i: a presence channel nobody is in");
        }
        // The daemon answers whatever is left. Then the gateway and the daemon must agree on who is in which room.
        $sync();
        foreach (array_keys($outstanding) as $request) {
            $answer($request, StatusCode::OK);
            $sync();
        }
        $s = $gw->stats();
        assertSame(0, $s->joinsInProgress, "seed $seed: every join was answered");
        $held = 0;
        foreach ($open as $conn => $_) {
            assertSame(count($rooms[$conn] ?? []), $gw->channelsOf($conn), "seed $seed: connection $conn is in as many rooms as the daemon thinks");
            $held += count($rooms[$conn] ?? []);
        }
        assertSame($held, $s->channels, "seed $seed: the gateway and the daemon hold the same memberships");
        foreach (array_keys($open) as $conn) {
            $gw->closed($conn);
        }
        $s = $gw->stats();
        assertSame([0, 0, 0, 0, 0], [$s->connections, $s->channels, $s->joinsInProgress, $s->presenceChannels, $s->presenceMembers], "seed $seed: nothing survives its connections");
    }
});

// ---- client events (whispers) ---------------------------------------------------------------------

function whisper(string $channel, string $event, mixed $data): string
{
    return json_encode(['event' => $event, 'channel' => $channel, 'data' => $data]);
}

test('pusher gateway: a member of a private channel may whisper to the others, and not to itself', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'private-chat.1');
    $link->published = [];
    $gw->received(1, whisper('private-chat.1', 'client-typing', ['user' => 'ada']));
    assertSame(1, count($link->published));
    [$topic, $payload, $except] = $link->published[0];
    assertSame('private-chat.1', $topic);
    assertSame(1, $except);
    assertSame(['event' => 'client-typing', 'channel' => 'private-chat.1', 'data' => ['user' => 'ada']], json_decode($payload, true));
});

test('pusher gateway: a whisper on a presence channel carries the sender user id', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'presence-room', user_data('7'));
    $link->published = [];
    $gw->received(1, whisper('presence-room', 'client-hi', new stdClass()));
    $ev = json_decode($link->published[0][1], true);
    assertSame('7', $ev['user_id']);
    assertTrue(str_contains($link->published[0][1], '"data":{}'), 'an empty object stays an object');
});

test('pusher gateway: a whisper from a non-member, on a public channel, or with bad data is refused', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $s2 = open_conn($gw, $link, 2);
    join_channel($gw, $link, 1, $s1, 'private-a');
    join_channel($gw, $link, 1, $s1, 'public-a');
    $link->published = [];
    $gw->received(2, whisper('private-a', 'client-x', ['a' => 1]));
    assertSame(4009, $link->lastFrame(2)['data']['code'], 'not a member');
    $gw->received(1, whisper('public-a', 'client-x', ['a' => 1]));
    assertSame(4009, $link->lastFrame(1)['data']['code'], 'public channel');
    $gw->received(1, whisper('never-joined', 'client-x', ['a' => 1]));
    assertSame(4009, $link->lastFrame(1)['data']['code'], 'not joined');
    $gw->received(1, whisper('private-a', 'client-x', 'a string'));
    assertSame(4200, $link->lastFrame(1)['data']['code'], 'data not an object');
    $gw->received(1, json_encode(['event' => 'client-x', 'data' => ['a' => 1]]));
    assertSame(4200, $link->lastFrame(1)['data']['code'], 'no channel');
    $gw->received(1, whisper('private-a', 'client-', ['a' => 1]));
    $gw->received(1, whisper('private-a', 'clientx', ['a' => 1]));
    assertSame([], $link->published, 'none of those reached anyone');
});

test('pusher gateway: whispers can be switched off, and are rate limited', function () {
    [$gw, $link] = gateway(['clientEvents' => false]);
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'private-a');
    $link->published = [];
    $gw->received(1, whisper('private-a', 'client-x', ['a' => 1]));
    $f = $link->lastFrame(1);
    assertSame(4301, $f['data']['code']);
    assertSame('The app does not have client messaging enabled.', $f['data']['message']);
    assertSame([], $link->published);

    [$gw, $link, $clock] = gateway(['clientEventsPerSecond' => 2, 'maxStrikes' => 100]);
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'private-a');
    $link->published = [];
    for ($i = 0; $i < 5; $i++) {
        $gw->received(1, whisper('private-a', 'client-x', ['i' => $i]));
    }
    assertSame(2, count($link->published));
    assertSame(4301, $link->lastFrame(1)['data']['code']);
    $clock->now += 1.0;
    $gw->received(1, whisper('private-a', 'client-x', ['i' => 9]));
    assertSame(3, count($link->published));
});

test('pusher gateway: a whisper over its rate is refused with an error and costs no strike (as Reverb does)', function () {
    // A fast typist whose application does not throttle `typing` must not be disconnected for it.
    [$gw, $link] = gateway(['clientEventsPerSecond' => 1, 'maxStrikes' => 2]);
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'private-a');
    for ($i = 0; $i < 50; $i++) {
        $gw->received(1, whisper('private-a', 'client-x', ['i' => $i]));
    }
    assertSame(4301, $link->lastFrame(1)['data']['code']);
    assertSame([], $link->closed, 'the connection is still open');
});

test('pusher gateway: a whisper cannot forge the fields the server adds', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'presence-room', user_data('7'));
    $link->published = [];
    $text = json_encode([
        'event' => 'client-x', 'channel' => 'presence-room', 'user_id' => 'admin',
        'data' => ['a' => 1], 'extra' => 'nope',
    ]);
    $gw->received(1, $text);
    $ev = json_decode($link->published[0][1], true);
    assertSame('7', $ev['user_id'], 'the user id is the signed one');
    assertTrue(!isset($ev['extra']), 'unknown fields are dropped');
});

// ---- the strict app key ---------------------------------------------------------------------------

test('pusher gateway: the app key in the URL can be checked at the handshake', function () {
    [$gw] = gateway();
    assertTrue($gw->acceptsPath('/app/' . GW_KEY));
    assertTrue($gw->acceptsPath('/prefix/app/' . GW_KEY));
    assertTrue(!$gw->acceptsPath('/app/other'));
    assertTrue(!$gw->acceptsPath('/app/' . GW_KEY . 'x'));
    assertTrue(!$gw->acceptsPath('/'));
    assertTrue(!$gw->acceptsPath(''));
});

// ---- values that decode but cannot be encoded again (found by the commit security review) --------------

test('pusher gateway: a whisper with a number JSON cannot re-encode (1e999 decodes to INF) is a bad frame, not an exception', function () {
    [$gw, $link] = gateway(['maxStrikes' => 100]);
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'private-a');
    $link->published = [];
    foreach ([
        '{"event":"client-x","channel":"private-a","data":{"n":1e999}}',
        '{"event":"client-x","channel":"private-a","data":"{\"n\":1e999}"}',
        '{"event":"client-x","channel":"private-a","data":{"deep":{"n":[-1e999]}}}',
    ] as $text) {
        $gw->received(1, $text);
        assertSame(4200, $link->lastFrame(1)['data']['code'], $text);
    }
    assertSame([], $link->published);
});

test('pusher gateway: presence user_info that cannot be encoded again is refused when the member is read', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $data = '{"user_id":"1","user_info":{"score":1e999}}';
    $gw->received(1, frame_text('pusher:subscribe', [
        'channel' => 'presence-room', 'channel_data' => $data,
        'auth' => Signature::authorize(GW_KEY, GW_SECRET, $s1, 'presence-room', $data),
    ]));
    assertSame(4200, $link->lastFrame(1)['data']['code']);
    assertSame([], $link->subscribed);
});

// ---- leaving before the daemon has answered the join (pusher-js itself waits for the success first, but
// ---- another client may send the unsubscribe early) ---------------------------------------------------------

test('pusher gateway: unsubscribing before the daemon answers cancels the join and leaves the room again', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $s1));
    $request = $link->lastRequest();
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'orders']));
    $before = count($link->frames(1));
    $gw->status($request, StatusCode::OK);
    assertSame([[1, 'orders']], $link->unsubscribed, 'the daemon room is left');
    assertSame($before, count($link->frames(1)), 'no subscription_succeeded for a channel that was left');
    assertSame(0, $gw->channelsOf(1));
    assertSame(0, $gw->pendingCount());
});

test('pusher gateway: a presence join that was left before the answer never announces the member', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $s2 = open_conn($gw, $link, 2);
    join_channel($gw, $link, 2, $s2, 'presence-room', user_data('2'));
    $link->published = [];
    $gw->received(1, subscribe_frame('presence-room', $s1, user_data('1')));
    $request = $link->lastRequest();
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'presence-room']));
    $gw->status($request, StatusCode::OK);
    assertSame([], $link->published, 'nobody is told about a member who already left');
    assertSame(1, $link->lastFrame(2)['data']['presence']['count'] ?? 1);
    $gw->closed(1);
    assertSame([], $link->published, 'and nobody is told that it left');
    assertSame(1, $gw->presenceChannels());
});

test('pusher gateway: leaving and joining again before the answer ends in one join', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $s1));
    $request = $link->lastRequest();
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'orders']));
    $gw->received(1, subscribe_frame('orders', $s1));
    assertSame(1, count($link->subscribed), 'the daemon is not asked twice');
    $gw->status($request, StatusCode::OK);
    assertSame([], $link->unsubscribed);
    assertSame('pusher_internal:subscription_succeeded', $link->lastFrame(1)['event']);
    assertSame(1, $gw->channelsOf(1));
});

test('pusher gateway: a cancelled join that the daemon refuses says nothing', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('orders', $s1));
    $request = $link->lastRequest();
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'orders']));
    $before = count($link->frames(1));
    $gw->status($request, StatusCode::LIMIT);
    assertSame($before, count($link->frames(1)));
    assertSame([], $link->unsubscribed, 'the connection was never in the room');
});

test('pusher gateway: re-joining a presence channel after a cancelled join uses the new signed member', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    $gw->received(1, subscribe_frame('presence-room', $s1, user_data('1')));
    $request = $link->lastRequest();
    $gw->received(1, frame_text('pusher:unsubscribe', ['channel' => 'presence-room']));
    $gw->received(1, subscribe_frame('presence-room', $s1, user_data('5')));
    $gw->status($request, StatusCode::OK);
    assertSame(['5'], $link->lastFrame(1)['data']['presence']['ids']);
});

// ---- encrypted channels carry no client events -----------------------------------------------------------

test('pusher gateway: client events are refused on an encrypted channel, as Pusher refuses them', function () {
    [$gw, $link] = gateway();
    $s1 = open_conn($gw, $link, 1);
    join_channel($gw, $link, 1, $s1, 'private-encrypted-chat.1');
    $link->published = [];
    $gw->received(1, whisper('private-encrypted-chat.1', 'client-x', ['a' => 1]));
    $f = $link->lastFrame(1);
    assertSame('pusher:error', $f['event']);
    assertSame(4301, $f['data']['code']);
    assertSame([], $link->published);
});

test('pusher gateway: user authentication (pusher:signin) is answered once and costs no strike', function () {
    [$gw, $link] = gateway(['maxStrikes' => 2]);
    open_conn($gw, $link, 1);
    for ($i = 0; $i < 5; $i++) {
        $gw->received(1, frame_text('pusher:signin', ['auth' => 'x', 'user_data' => '{}']));
    }
    $f = $link->lastFrame(1);
    assertSame('pusher:error', $f['event']);
    assertSame(4200, $f['data']['code']);
    assertSame('User authentication is not supported.', $f['data']['message']);
    assertSame([], $link->closed, 'a legitimate client is not closed for asking');
});
