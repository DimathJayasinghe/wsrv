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
use Wsrv\WsrvException;

/** @return array{0: resource, 1: resource} [application end, fake daemon end] */
function socket_pair(): array
{
    // Windows has no Unix socket pair for PHP: stream_socket_pair() accepts only STREAM_PF_INET there.
    $domain = PHP_OS_FAMILY === 'Windows' ? STREAM_PF_INET : STREAM_PF_UNIX;
    $pair = stream_socket_pair($domain, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
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

test('client: a refusal sent by a daemon that hangs up at once is still reported as a refusal', function () {
    // wsrvd answers a second application with one STATUS and closes without reading our HELLO. If
    // the close wins the race, our HELLO write fails (broken pipe), but the STATUS is already in
    // our receive buffer and says why: it must not be thrown away as "connection closed".
    if (PHP_OS_FAMILY === 'Windows') {
        // Not a client bug: on Windows the socket pair is TCP, and a peer that closes while our HELLO is
        // still unread makes the OS send RST, which Windows answers by discarding what we had already
        // received. The Supervisor retries a hung-up daemon and BACKPRESSURE alike (ADR-0038 section 7).
        skip('Windows drops received data when a TCP peer resets the connection');
    }
    [$app, $daemon] = socket_pair();
    fwrite($daemon, status_frame(0, StatusCode::BACKPRESSURE));
    fclose($daemon);
    $e = assertThrows(HandshakeRefused::class, fn () => Client::handshake($app, '', 1.0));
    assertSame(StatusCode::BACKPRESSURE, $e->status);
});

test('client: a daemon that hangs up without a word is ConnectionClosed', function () {
    [$app, $daemon] = socket_pair();
    fclose($daemon);
    assertThrows(ConnectionClosed::class, fn () => Client::handshake($app, '', 1.0));
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

// ---- the TCP transport (ADR-0038) ---------------------------------------------------------------
// Real loopback TCP, no fork and no pcntl, so the same tests run on Windows.

/** @return array{0: resource, 1: string} [listening socket, its "tcp://127.0.0.1:PORT" endpoint] */
function tcp_daemon(): array
{
    $server = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
    $server !== false || fail('cannot listen: ' . $error);
    $name = stream_socket_get_name($server, false);
    is_string($name) || fail('no local name');
    return [$server, 'tcp://' . $name];
}

test('client: a tcp:// endpoint is dialled and carries the handshake', function () {
    [$server, $endpoint] = tcp_daemon();
    $stream = Client::dial($endpoint, 2.0);
    $peer = stream_socket_accept($server, 2.0);
    $peer !== false || fail('daemon did not get the connection');
    fwrite($peer, hello_ok(65536));
    $client = Client::handshake($stream, 'a-token-of-some-length', 2.0);
    assertSame(65536, $client->maxFrame());
    stream_set_blocking($peer, false);
    assertSame(Codec::hello('a-token-of-some-length'), received($peer));
});

test('client: connect() to a tcp:// endpoint completes the handshake with a daemon in another process', function () {
    // connect() waits for HELLO_OK, so the daemon must answer while connect() is waiting: a child PHP
    // process plays it (proc_open exists on Windows too; pcntl_fork does not).
    $process = proc_open(
        [PHP_BINARY, __DIR__ . '/tcp_fake_daemon.php', 'a-token-of-some-length'],
        [1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
        $pipes,
    );
    is_resource($process) || fail('cannot start the fake daemon');
    try {
        $endpoint = trim((string) fgets($pipes[1]));
        assertTrue(str_starts_with($endpoint, 'tcp://127.0.0.1:'), 'the fake daemon announced its endpoint: ' . $endpoint);
        $client = Client::connect($endpoint, 'a-token-of-some-length', 5.0);
        assertSame(4096, $client->maxFrame(), 'HELLO_OK from the other process arrived over TCP');
        $client->close();
        $report = trim((string) stream_get_contents($pipes[1]));
        assertSame('hello-ok-sent token-matched', $report, 'the daemon saw our HELLO with the token');
    } finally {
        proc_terminate($process);
        proc_close($process);
    }
});

test('client: a wrong token over TCP is HandshakeRefused with the daemon status', function () {
    [$server, $endpoint] = tcp_daemon();
    $stream = Client::dial($endpoint, 2.0);
    $peer = stream_socket_accept($server, 2.0);
    $peer !== false || fail('daemon did not get the connection');
    fwrite($peer, status_frame(0, StatusCode::INVALID_ARGUMENT));
    $e = assertThrows(HandshakeRefused::class, fn () => Client::handshake($stream, 'wrong', 2.0));
    assertSame(StatusCode::INVALID_ARGUMENT, $e->status);
});

test('client: a TCP endpoint that nobody listens on is a clear WsrvException, not a hang', function () {
    [$server, $endpoint] = tcp_daemon();
    fclose($server);  // the port is now closed
    $started = microtime(true);
    $e = assertThrows(WsrvException::class, fn () => Client::connect($endpoint, 'x', 1.0));
    assertTrue(str_contains($e->getMessage(), $endpoint), 'names the endpoint: ' . $e->getMessage());
    assertTrue(microtime(true) - $started < 3.0, 'returned promptly');
});

test('client: the TCP socket is told not to wait for more data before sending (TCP_NODELAY)', function () {
    // The IPC link carries small request and answer frames; Nagle's algorithm plus delayed ACK
    // would add tens of milliseconds to each (ADR-0038 section 7).
    assertSame(true, Client::tcpContext()['socket']['tcp_nodelay'] ?? null);
    if (function_exists('socket_import_stream') && defined('TCP_NODELAY')) {
        [$server, $endpoint] = tcp_daemon();
        $stream = Client::dial($endpoint, 2.0);
        $socket = socket_import_stream($stream);
        $socket !== false || fail('cannot inspect the socket');
        assertTrue(socket_get_option($socket, SOL_TCP, TCP_NODELAY) !== 0, 'TCP_NODELAY is on the real socket');
    }
});

test('client: endpoints the client must refuse before opening anything', function () {
    foreach (['tcp://', 'tcp://127.0.0.1', 'tcp://127.0.0.1:', 'tcp://:9300', 'tcp://127.0.0.1:0',
              'tcp://127.0.0.1:65536', 'tcp://127.0.0.1:-1', 'tcp://127.0.0.1:99999999999999999999',
              'tcp://[::1', 'tcp://host name:80', "tcp://127.0.0.1:80\n", 'tcp://127.0.0.1:80/path',
              'tcp://user@127.0.0.1:80', 'tcp://[::1]'] as $bad) {
        $e = assertThrows(WsrvException::class, fn () => Client::dial($bad, 0.1), $bad);
        // The validation message, not "cannot connect": nothing may be opened for a malformed endpoint.
        assertTrue(str_contains($e->getMessage(), 'expected tcp://host:port'), 'refused by validation: ' . $e->getMessage());
    }
});

test('client: IPv4, IPv6 and names are accepted in the endpoint form', function () {
    foreach (['tcp://127.0.0.1:9300', 'tcp://[::1]:9300', 'tcp://localhost:9300', 'tcp://wsrvd.internal:1', 'tcp://10.0.0.7:65535'] as $good) {
        assertSame(true, Client::isTcpEndpoint($good), $good);
    }
    assertSame(false, Client::isTcpEndpoint('/run/wsrv/ipc.sock'));
    assertSame(false, Client::isTcpEndpoint('C:\\wsrv\\ipc.sock'));
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
        skip('needs ext-pcntl to run the fake daemon as a second process');
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
