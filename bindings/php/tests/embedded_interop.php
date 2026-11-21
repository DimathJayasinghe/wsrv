<?php
// SPDX-License-Identifier: Apache-2.0
// End to end: the REAL libwsrv.so through FFI (Wsrv\Embedded\Server) and a raw WebSocket client.
// Both run in this one PHP thread: the library's I/O threads do the sockets, so the client can write,
// the server can step() and the client can read, in turn.
// Run with: php tests/embedded_interop.php /path/to/libwsrv.so

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';
require __DIR__ . '/ws_client.php';

use Wsrv\Embedded\Connection;
use Wsrv\Embedded\Server;
use Wsrv\WsrvException;

$library = $argv[1] ?? fail('usage: php tests/embedded_interop.php /path/to/libwsrv.so');
extension_loaded('FFI') || fail('ext-ffi is not loaded: the embedded binding needs it');

// Closures here capture by reference (`use (&$x)`): an arrow function would freeze the value.
/** Step the server until $done() or the deadline; a test that waits for nothing would hide a hang. */
function pump(Server $server, callable $done, float $seconds = 5.0): void
{
    $deadline = microtime(true) + $seconds;
    while (!$done()) {
        microtime(true) < $deadline || fail('timed out waiting for the server');
        $server->step(50);
    }
}

$server = new Server(['listen_port' => 0, 'allow_origin' => ['https://app.example.com']], $library);
$log = [];
$server->on('open', function (Connection $c, string $peer) use (&$log) {
    $log[] = 'open';
});
$server->on('message', function (Connection $c, string $data, bool $binary) use (&$log) {
    $log[] = $binary ? 'binary' : 'text';
    $binary ? $c->sendBinary($data) : $c->send($data);
});
$server->on('close', function (Connection $c, int $code, string $reason, bool $clean) use (&$log) {
    $log[] = "close:$code:" . ($clean ? 'clean' : 'abrupt');
});
$server->start();
$port = $server->port();
assertTrue($port > 0, 'the kernel chose a port');

// 1. open, text echo, binary echo, a clean close with a code
$client = ws_connect($port);
pump($server, function () use (&$log) { return $log === ['open']; });
ws_send($client, 1, 'hello');
pump($server, function () use (&$log) { return count($log) === 2; });
assertSame([1, 'hello'], ws_read($client), 'text echo');
$bytes = "\x00\xff\x10 binary";
ws_send($client, 2, $bytes);
pump($server, function () use (&$log) { return count($log) === 3; });
assertSame([2, $bytes], ws_read($client), 'binary echo');
ws_send($client, 8, pack('n', 1000) . 'bye');
pump($server, function () use (&$log) { return count($log) === 4; });
assertSame(['open', 'text', 'binary', 'close:1000:clean'], $log, 'event order');
fclose($client);

// 2. the server closes with its own code
$log = [];
$closeFrom = null;
$server->on('open', function (Connection $c, string $peer) use (&$closeFrom) {
    $closeFrom = $c;
    $c->close(4001, 'enough');
});
$client = ws_connect($port);
pump($server, function () use (&$closeFrom) { return $closeFrom !== null; });
[$op, $payload] = ws_read($client);
assertSame(8, $op, 'a close frame');
assertSame(4001, unpack('n', $payload)[1], 'the code the application chose');
assertSame('enough', substr($payload, 2), 'the reason');
fclose($client);

// 3. a send to a connection that has ended is ConnectionClosed, never a crash
pump($server, function () use (&$log) { return count($log) >= 1; });
assertThrows(\Wsrv\ConnectionClosed::class, fn () => $closeFrom->send('late'), 'send after close');

// 4. text that is not UTF-8 is refused before anything is sent
$bad = null;
$server->on('open', function (Connection $c, string $peer) use (&$bad) {
    $bad = $c;
});
$client = ws_connect($port);
pump($server, function () use (&$bad) { return $bad !== null; });
assertThrows(\InvalidArgumentException::class, fn () => $bad->send("\xff\xfe"), 'invalid UTF-8');
fclose($client);

// 5. a run() that is stopped from a handler returns
$stopped = false;
$server->on('message', function (Connection $c, string $data, bool $binary) use ($server, &$stopped) {
    $stopped = true;
    $server->stop();
});
$client = ws_connect($port);
ws_send($client, 1, 'stop');
$server->run();
assertTrue($stopped, 'the handler ran and run() returned');
fclose($client);

// 6. a wrong configuration is named, not ignored; a library that is not there is a clear error
$e = assertThrows(WsrvException::class, fn () => (new Server(['no_such_key' => '1'], $library))->start(), 'unknown key');
assertTrue(str_contains($e->getMessage(), 'no_such_key') && str_contains($e->getMessage(), 'INVALID_CONFIG'), 'the message names the key and the status: ' . $e->getMessage());
$e = assertThrows(WsrvException::class, fn () => new Server([], '/nonexistent/libwsrv.so'), 'missing library');
assertTrue(str_contains($e->getMessage(), '/nonexistent/libwsrv.so'), 'the message names the path');
$e = assertThrows(WsrvException::class, fn () => (new Server(['handshake_hook' => true], $library))->start(), 'handshake_hook');
assertTrue(str_contains($e->getMessage(), 'handshake_hook'), 'refused up front, not by silently hanging every upgrade');

// 7. start() refuses to run twice, and the server can be stopped and destroyed
assertThrows(WsrvException::class, fn () => $server->start(), 'second start');
$server->close();
echo "embedded interop: ok\n";
