<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Client;
use Wsrv\ConnectionClosed;
use Wsrv\HandshakeRefused;
use Wsrv\Ipc\Codec;
use Wsrv\Ipc\Event;
use Wsrv\Ipc\EventKind;
use Wsrv\Ipc\Pong;
use Wsrv\Ipc\StatusReply;
use Wsrv\ProtocolError;
use Wsrv\StatusCode;

/** @return array{0: resource, 1: resource} [application end, fake daemon end] */
function socket_pair(): array
{
    $pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
    $pair !== false || fail('stream_socket_pair failed');
    stream_set_blocking($pair[1], false);
    return $pair;
}

function hello_ok(int $maxFrame = 1 << 20, int $major = 0): string
{
    return pack('NCC', 10, 2, 0) . pack('nnN', $major, 1, $maxFrame);
}

function status_frame(int $request, int $status): string
{
    return pack('NCC', 7, 9, 0) . pack('NC', $request, $status);
}

function event_bytes(int $kind, int $conn, string $data, int $code = 0, int $clean = 0): string
{
    $body = pack('CJnC', $kind, $conn, $code, $clean) . $data;
    return pack('NCC', 2 + strlen($body), 8, 0) . $body;
}

/** Everything the fake daemon has received so far. */
function received($daemonEnd): string
{
    $all = '';
    while (($chunk = fread($daemonEnd, 65536)) !== false && $chunk !== '') {
        $all .= $chunk;
    }
    return $all;
}

/** A connected client whose fake daemon has already said HELLO_OK. @return array{0: Client, 1: resource} */
function ready_client(int $maxFrame = 1 << 20): array
{
    [$app, $daemon] = socket_pair();
    fwrite($daemon, hello_ok($maxFrame));
    $client = Client::handshake($app, '', 1.0);
    received($daemon);  // discard our HELLO
    return [$client, $daemon];
}

test('client: handshake sends HELLO with the token and learns the frame limit', function () {
    [$app, $daemon] = socket_pair();
    fwrite($daemon, hello_ok(65536));
    $client = Client::handshake($app, 'secret', 1.0);
    assertSame(65536, $client->maxFrame());
    assertSame(Codec::hello('secret'), received($daemon));
});

test('client: a refused handshake raises HandshakeRefused and closes the socket', function () {
    [$app, $daemon] = socket_pair();
    fwrite($daemon, status_frame(0, StatusCode::INVALID_ARGUMENT));
    $e = assertThrows(HandshakeRefused::class, fn () => Client::handshake($app, 'wrong', 1.0));
    assertSame(StatusCode::INVALID_ARGUMENT, $e->status);
    assertTrue(!is_resource($app) || feof($app) || fread($daemon, 1) !== false, 'socket released');
});

test('client: another application already connected is BACKPRESSURE', function () {
    [$app, $daemon] = socket_pair();
    fwrite($daemon, status_frame(0, StatusCode::BACKPRESSURE));
    $e = assertThrows(HandshakeRefused::class, fn () => Client::handshake($app, '', 1.0));
    assertSame(StatusCode::BACKPRESSURE, $e->status);
});

test('client: no answer within the timeout is ConnectionClosed', function () {
    [$app, $daemon] = socket_pair();
    $started = microtime(true);
    assertThrows(ConnectionClosed::class, fn () => Client::handshake($app, '', 0.2));
    assertTrue(microtime(true) - $started < 2.0, 'returned promptly');
});

test('client: a different protocol major is refused', function () {
    [$app, $daemon] = socket_pair();
    fwrite($daemon, hello_ok(1 << 20, 1));
    assertThrows(ProtocolError::class, fn () => Client::handshake($app, '', 1.0));
});

test('client: anything but HELLO_OK or STATUS before the handshake is a protocol error', function () {
    [$app, $daemon] = socket_pair();
    fwrite($daemon, event_bytes(1, 1, 'x'));
    assertThrows(ProtocolError::class, fn () => Client::handshake($app, '', 1.0));
});

test('client: a daemon that hangs up during the handshake is ConnectionClosed', function () {
    [$app, $daemon] = socket_pair();
    fclose($daemon);
    assertThrows(ConnectionClosed::class, fn () => Client::handshake($app, '', 1.0));
});

test('client: commands are numbered from 1 and written as frames', function () {
    [$client, $daemon] = ready_client();
    assertSame(1, $client->sendText(5, 'hi'));
    assertSame(2, $client->sendBinary(5, "\x00\x01"));
    assertSame(3, $client->closeConnection(5, 4000, 'bye'));
    $expected = Codec::sendText(1, 5, 'hi') . Codec::sendBinary(2, 5, "\x00\x01") . Codec::close(3, 5, 4000, 'bye');
    assertSame($expected, received($daemon));
});

test('client: request ids wrap from 4294967295 to 1, never 0', function () {
    [$client, $daemon] = ready_client();
    $property = new ReflectionProperty(Client::class, 'nextRequest');
    $property->setValue($client, 0xFFFFFFFF);
    assertSame(0xFFFFFFFF, $client->sendText(1, 'a'));
    assertSame(1, $client->sendText(1, 'b'));
});

test('client: a message larger than the daemon accepts is refused locally', function () {
    [$client, $daemon] = ready_client(4096);
    assertThrows(ProtocolError::class, fn () => $client->sendBinary(1, str_repeat('x', 5000)));
    assertSame('', received($daemon), 'nothing may reach the daemon');
    $client->sendBinary(1, str_repeat('x', 4096 - 14));  // exactly at the limit: 2 + 12 + payload
    assertTrue(received($daemon) !== '');
});

test('client: poll hands out messages in order, then null on timeout', function () {
    [$client, $daemon] = ready_client();
    fwrite($daemon, event_bytes(1, 7, 'addr') . status_frame(1, 0) . pack('NCC', 2, 7, 0));
    $a = $client->poll(1.0);
    $b = $client->poll(1.0);
    $c = $client->poll(1.0);
    assertTrue($a instanceof Event && $a->kind === EventKind::Open && $a->conn === 7);
    assertTrue($b instanceof StatusReply && $b->request === 1);
    assertTrue($c instanceof Pong);
    $started = microtime(true);
    assertSame(null, $client->poll(0.15));
    assertTrue(microtime(true) - $started >= 0.1, 'waited');
});

test('client: poll(0) never blocks', function () {
    [$client, $daemon] = ready_client();
    $started = microtime(true);
    assertSame(null, $client->poll(0.0));
    assertTrue(microtime(true) - $started < 0.1);
});

test('client: a daemon that hangs up is ConnectionClosed', function () {
    [$client, $daemon] = ready_client();
    fclose($daemon);
    assertThrows(ConnectionClosed::class, fn () => $client->poll(1.0));
    assertThrows(ConnectionClosed::class, fn () => $client->sendText(1, 'x'), 'after close');
});

test('client: garbage from the daemon is a ProtocolError', function () {
    [$client, $daemon] = ready_client();
    fwrite($daemon, hex('00 00 00 02 c8 00'));
    assertThrows(ProtocolError::class, fn () => $client->poll(1.0));
});

test('client: ping is answered by a Pong message', function () {
    [$client, $daemon] = ready_client();
    $client->ping();
    assertSame(Codec::ping(), received($daemon));
    fwrite($daemon, pack('NCC', 2, 7, 0));
    assertTrue($client->poll(1.0) instanceof Pong);
});

test('client: a burst of commands does not deadlock against a daemon that waits for us to read', function () {
    if (!function_exists('pcntl_fork')) {
        return;  // cannot fork here; the scenario needs a second process
    }
    // The daemon writes a lot of events and only THEN starts reading our command. A client that
    // wrote without reading would block forever (both socket buffers full): this is the situation
    // wsrvd creates when it stops reading from a worker whose output it cannot drain.
    $events = 2000;
    $payload = 4 * 1024 * 1024;
    [$app, $daemon] = socket_pair();
    stream_set_blocking($daemon, true);
    $pid = pcntl_fork();
    if ($pid === 0) {
        fwrite($daemon, hello_ok(8 * 1024 * 1024));
        for ($i = 0; $i < $events; $i++) {
            fwrite($daemon, event_bytes(2, 1, str_repeat('e', 200)));
        }
        $expect = strlen(Codec::hello('')) + strlen(Codec::sendBinary(1, 1, str_repeat('b', $payload)));
        for ($got = 0; $got < $expect;) {
            $chunk = fread($daemon, 65536);
            if ($chunk === false || $chunk === '') {
                exit(2);
            }
            $got += strlen($chunk);
        }
        fwrite($daemon, event_bytes(2, 1, 'done'));
        exit(0);
    }
    $client = Client::handshake($app, '', 5.0);
    $client->setWriteTimeout(10.0);
    $client->sendBinary(1, str_repeat('b', $payload));
    $seen = 0;
    for ($m = $client->poll(5.0); $m instanceof Event; $m = $client->poll(5.0)) {
        $seen++;
        if ($m->data === 'done') {
            break;
        }
    }
    pcntl_waitpid($pid, $status);
    assertSame($events + 1, $seen, 'every event arrived, in spite of the blocked write');
    assertSame(0, pcntl_wexitstatus($status), 'the daemon side read the whole command');
});
