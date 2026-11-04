<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Client;
use Wsrv\Ipc\Codec;
use Wsrv\Ipc\HandshakeRequest;
use Wsrv\ProtocolError;
use Wsrv\Upgrade;
use Wsrv\Worker;

/**
 * The question blob (docs/IPC_PROTOCOL.md section 9), written by hand: every string is a u32 length
 * and its bytes; two u16 counts introduce the lists.
 *
 * @param list<string> $offered
 * @param list<array{0:string,1:string}> $headers
 */
function handshake_blob(
    string $peer = '127.0.0.1:5000',
    string $path = '/chat',
    string $query = '',
    string $origin = 'https://app.example.com',
    string $host = 'chat.example.com',
    string $subprotocol = '',
    array $offered = [],
    array $headers = [],
): string {
    $str = static fn (string $s): string => pack('N', strlen($s)) . $s;
    $blob = $str($peer) . $str($path) . $str($query) . $str($origin) . $str($host) . $str($subprotocol);
    $blob .= pack('n', count($offered)) . implode('', array_map($str, $offered));
    $blob .= pack('n', count($headers));
    foreach ($headers as [$name, $value]) {
        $blob .= $str($name) . $str($value);
    }
    return $blob;
}

// ---- parsing the blob ------------------------------------------------------------------------

test('handshake request: every field is read', function () {
    $r = HandshakeRequest::parse(handshake_blob(
        query: 'token=abc&x=1',
        path: '/room/7',
        subprotocol: 'chat',
        offered: ['chat', 'v2'],
        headers: [['Cookie', 'a=1; session=good'], ['Cookie', 'b=2'], ['Authorization', 'Bearer t']],
    ));
    assertSame('127.0.0.1:5000', $r->peer);
    assertSame('/room/7', $r->path);
    assertSame('token=abc&x=1', $r->query);
    assertSame('https://app.example.com', $r->origin);
    assertSame('chat.example.com', $r->host);
    assertSame('chat', $r->subprotocol);
    assertSame(['chat', 'v2'], $r->offered);
    assertSame(3, count($r->headers));
});

test('handshake request: header lookup ignores case and keeps every value in order', function () {
    $r = HandshakeRequest::parse(handshake_blob(headers: [['Cookie', 'a=1'], ['X-Other', 'z'], ['cookie', 'b=2']]));
    assertSame(['a=1', 'b=2'], $r->headers('COOKIE'));
    assertSame('a=1', $r->header('cookie'));
    assertSame(null, $r->header('Authorization'));
    assertSame([], $r->headers('Authorization'));
});

test('handshake request: a cookie is found by name across every Cookie header', function () {
    $r = HandshakeRequest::parse(handshake_blob(headers: [
        ['Cookie', 'theme=dark; session=good'],
        ['Cookie', ' other=x ;last=1'],
    ]));
    assertSame('good', $r->cookie('session'));
    assertSame('x', $r->cookie('other'));
    assertSame('1', $r->cookie('last'));
    assertSame(null, $r->cookie('sess'), 'a name is matched whole, not as a prefix');
    assertSame(null, $r->cookie('missing'));
    $quoted = HandshakeRequest::parse(handshake_blob(headers: [['Cookie', 'a="x y"; b=']]));
    assertSame('"x y"', $quoted->cookie('a'), 'the value is returned as sent');
    assertSame('', $quoted->cookie('b'));
});

test('handshake request: values may hold any byte', function () {
    $odd = "a\x00b\xff\r\n";
    $r = HandshakeRequest::parse(handshake_blob(query: $odd));
    assertSame($odd, $r->query);
});

test('handshake request: empty fields and lists are fine', function () {
    $r = HandshakeRequest::parse(handshake_blob('', '', '', '', '', '', [], []));
    assertSame(['', [], []], [$r->origin, $r->offered, $r->headers]);
});

test('handshake request: a truncated, over-long or padded blob is a protocol error', function () {
    $blob = handshake_blob(offered: ['chat'], headers: [['Cookie', 'a=1']]);
    for ($cut = 0; $cut < strlen($blob); $cut++) {
        assertThrows(ProtocolError::class, fn () => HandshakeRequest::parse(substr($blob, 0, $cut)), "cut at $cut");
    }
    assertThrows(ProtocolError::class, fn () => HandshakeRequest::parse($blob . "\x00"), 'trailing byte');
    // A length that claims more than the buffer holds must not be trusted.
    assertThrows(ProtocolError::class, fn () => HandshakeRequest::parse(pack('N', 0xFFFFFFFF) . 'abc'));
});

// ---- answering ---------------------------------------------------------------------------------

test('upgrade: accept and reject send the right frames, once', function () {
    [$client, $daemon] = ready_client();
    $request = HandshakeRequest::parse(handshake_blob(offered: ['chat']));
    $accepted = new Upgrade($client, 5, $request);
    assertSame(false, $accepted->answered());
    assertSame(1, $accepted->accept('chat'));
    assertSame(true, $accepted->answered());
    assertSame(Codec::handshakeAccept(1, 5, 'chat'), received($daemon));
    assertThrows(LogicException::class, fn () => $accepted->reject(403), 'a second answer is a bug');
    assertSame('', received($daemon), 'and nothing was sent for it');

    $refused = new Upgrade($client, 6, $request);
    assertSame(2, $refused->reject(401));
    assertSame(Codec::handshakeReject(2, 6, 401), received($daemon));
});

test('upgrade: a name the client did not offer is refused here, before any byte is sent', function () {
    [$client, $daemon] = ready_client();
    $upgrade = new Upgrade($client, 5, HandshakeRequest::parse(handshake_blob(offered: ['chat'])));
    assertThrows(InvalidArgumentException::class, fn () => $upgrade->accept('nope'));
    assertSame(false, $upgrade->answered(), 'the mistake does not use up the answer');
    assertSame('', received($daemon));
    assertSame(1, $upgrade->accept('chat'));
});

// ---- the worker --------------------------------------------------------------------------------

/** @return array{0: Worker, 1: resource} */
function hook_worker(): array
{
    [$client, $daemon] = ready_client();
    return [new Worker($client), $daemon];
}

function question(int $conn, string $blob): string
{
    return event_bytes(6, $conn, $blob);
}

test('worker authenticate: true accepts, false rejects with 403, an int rejects with that status', function () {
    foreach ([[true, null], [false, 403], [401, 401], [429, 429]] as [$returned, $status]) {
        [$worker, $daemon] = hook_worker();
        $worker->on('authenticate', fn (Upgrade $u) => $returned);
        fwrite($daemon, question(7, handshake_blob()));
        $worker->step(0.5);
        $want = $status === null ? Codec::handshakeAccept(1, 7) : Codec::handshakeReject(1, 7, $status);
        assertSame($want, received($daemon), var_export($returned, true));
    }
});

test('worker authenticate: a string accepts with that subprotocol', function () {
    [$worker, $daemon] = hook_worker();
    $worker->on('authenticate', fn (Upgrade $u) => 'v2');
    fwrite($daemon, question(7, handshake_blob(offered: ['chat', 'v2'])));
    $worker->step(0.5);
    assertSame(Codec::handshakeAccept(1, 7, 'v2'), received($daemon));
});

test('worker authenticate: the handler sees the whole request and can check a cookie', function () {
    [$worker, $daemon] = hook_worker();
    $seen = null;
    $worker->on('authenticate', function (Upgrade $u) use (&$seen) {
        $seen = [$u->conn, $u->request->path, $u->request->cookie('session')];
        return $u->request->cookie('session') === 'good';
    });
    fwrite($daemon, question(3, handshake_blob(path: '/room/1', headers: [['Cookie', 'session=good']])));
    fwrite($daemon, question(4, handshake_blob(headers: [['Cookie', 'session=forged']])));
    $worker->step(0.5);
    assertSame([3, '/room/1', 'good'], $seen);
    $worker->step(0.5);
    assertSame(Codec::handshakeAccept(1, 3) . Codec::handshakeReject(2, 4, 403), received($daemon));
});

test('worker authenticate: null means "later"; the answer goes out when the handler gives it', function () {
    [$worker, $daemon] = hook_worker();
    $held = null;
    $worker->on('authenticate', function (Upgrade $u) use (&$held) {
        $held = $u;
        return null;
    });
    fwrite($daemon, question(9, handshake_blob()));
    $worker->step(0.5);
    assertSame('', received($daemon), 'nothing is sent while the application thinks');
    $held->reject(401);
    assertSame(Codec::handshakeReject(1, 9, 401), received($daemon));
});

test('worker authenticate: a handler that answered itself is not answered twice', function () {
    [$worker, $daemon] = hook_worker();
    $worker->on('authenticate', function (Upgrade $u) {
        $u->reject(429);
        return true;  // too late: the answer is already given
    });
    fwrite($daemon, question(9, handshake_blob()));
    $worker->step(0.5);
    assertSame(Codec::handshakeReject(1, 9, 429), received($daemon));
});

test('worker authenticate: without a handler every upgrade is refused with 503 (fail closed)', function () {
    [$worker, $daemon] = hook_worker();
    fwrite($daemon, question(9, handshake_blob()));
    $worker->step(0.5);
    assertSame(Codec::handshakeReject(1, 9, 503), received($daemon));
});

test('worker authenticate: a handler that throws refuses with 500 and the exception still surfaces', function () {
    [$worker, $daemon] = hook_worker();
    $worker->on('authenticate', function () {
        throw new RuntimeException('database is down');
    });
    fwrite($daemon, question(9, handshake_blob()));
    $e = assertThrows(RuntimeException::class, fn () => $worker->step(0.5));
    assertSame('database is down', $e->getMessage());
    assertSame(Codec::handshakeReject(1, 9, 500), received($daemon));
});

test('worker authenticate: an unusable return value refuses with 500 and is reported', function () {
    foreach ([302, 'not-offered', 3.5, ['x'], new stdClass()] as $bad) {
        [$worker, $daemon] = hook_worker();
        $worker->on('authenticate', fn () => $bad);
        fwrite($daemon, question(9, handshake_blob(offered: ['chat'])));
        assertThrows(UnexpectedValueException::class, fn () => $worker->step(0.5), var_export($bad, true));
        assertSame(Codec::handshakeReject(1, 9, 500), received($daemon), 'fail closed');
    }
});

test('worker authenticate: a corrupt question is refused with 503 and reported', function () {
    [$worker, $daemon] = hook_worker();
    $worker->on('authenticate', fn () => true);
    fwrite($daemon, question(9, 'garbage'));
    assertThrows(ProtocolError::class, fn () => $worker->step(0.5));
    assertSame(Codec::handshakeReject(1, 9, 503), received($daemon));
});

test('worker authenticate: a question is not a connection', function () {
    [$worker, $daemon] = hook_worker();
    $opened = 0;
    $worker->on('authenticate', fn () => true)->on('open', function () use (&$opened) {
        $opened++;
    });
    fwrite($daemon, question(9, handshake_blob()));
    $worker->step(0.5);
    assertSame([0, 0], [$worker->connectionCount(), $opened], 'only the Open event makes a connection');
    fwrite($daemon, event_bytes(1, 9, '127.0.0.1:5000'));
    $worker->step(0.5);
    assertSame([1, 1], [$worker->connectionCount(), $opened]);
});
