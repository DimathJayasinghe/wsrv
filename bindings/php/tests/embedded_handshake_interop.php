<?php
// SPDX-License-Identifier: Apache-2.0
// End to end: the handshake hook (ADR-0018) through the embedded binding (ADR-0025 A1): the REAL
// libwsrv.so, an `authenticate` handler, and a raw client. One PHP thread: the client sends the upgrade
// without waiting (ws_request_async), the server steps, the client then reads the answer.
// Run with: php tests/embedded_handshake_interop.php /path/to/libwsrv.so

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';
require __DIR__ . '/ws_client.php';

use Wsrv\Embedded\Connection;
use Wsrv\Embedded\Server;
use Wsrv\Embedded\Upgrade;
use Wsrv\WsrvException;

$library = $argv[1] ?? fail('usage: php tests/embedded_handshake_interop.php /path/to/libwsrv.so');
extension_loaded('FFI') || fail('ext-ffi is not loaded: the embedded binding needs it');

// Closures that wait on a variable capture it by reference (`use (&$x)`): an arrow function would freeze it.
function pump(Server $server, callable $done, float $seconds = 5.0): void
{
    $deadline = microtime(true) + $seconds;
    while (!$done()) {
        microtime(true) < $deadline || fail('timed out waiting for the server');
        $server->step(50);
    }
}

/** Send an upgrade with these headers, let the server decide, return the HTTP answer head. */
function upgrade(Server $server, int $port, string $headers, ?callable $done = null): string
{
    $pending = ws_request_async($port, $headers);
    pump($server, $done ?? fn () => true);
    // Whatever the handler decided has been handed to the library; the answer is on the wire.
    usleep(100_000);
    [, $head] = ws_finish($pending);
    return $head;
}

$seen = [];
$pendingUpgrade = null;
$server = new Server([
    'listen_port' => 0,
    'allow_origin' => ['https://app.example.com'],
    'handshake_hook' => true,
    'handshake_forward_header' => ['Cookie'],
], $library);
$mode = 'cookie';
$server->on('authenticate', function (Upgrade $u) use (&$seen, &$mode, &$pendingUpgrade) {
    $seen[] = $u->request;
    if ($mode === 'later') {
        $pendingUpgrade = $u;  // answer through it, within the decision timeout
        return null;
    }
    return match ($mode) {
        'cookie' => $u->request->cookie('session') === 'good',
        'status' => 401,
        'subprotocol' => 'superchat',
        'throw' => throw new RuntimeException('login service is down'),
        'garbage' => 'banana',
    };
});
$echoed = [];
$server->on('open', function (Connection $c, string $peer) use (&$echoed) {
    $echoed[] = 'open';
});
$server->on('message', function (Connection $c, string $data, bool $binary) {
    $c->send($data);
});
$server->start();
$port = $server->port();

// 1. a good cookie: the handler sees the request, the upgrade is accepted, the connection works
$head = upgrade($server, $port, "Cookie: session=good; theme=dark\r\n", function () use (&$seen) { return count($seen) === 1; });
assertTrue(str_starts_with($head, 'HTTP/1.1 101'), "accepted: $head");
$request = $seen[0];
assertSame('/chat', $request->path, 'path');
assertSame('https://app.example.com', $request->origin, 'origin');
assertSame('good', $request->cookie('session'), 'cookie');
assertSame('dark', $request->cookie('theme'), 'second cookie');
pump($server, function () use (&$echoed) { return count($echoed) === 1; });  // the accepted upgrade opens its connection

// 2. a wrong cookie: refused with 403, no open event
$seen = [];
$opensBefore = count($echoed);
$head = upgrade($server, $port, "Cookie: session=forged\r\n", function () use (&$seen) { return count($seen) === 1; });
assertTrue(str_starts_with($head, 'HTTP/1.1 403'), "false is 403: $head");
$server->step(50);
assertSame($opensBefore, count($echoed), 'a refused upgrade produces no open event');

// 3. a status from the handler
$mode = 'status';
$seen = [];
$head = upgrade($server, $port, "Cookie: session=good\r\n", function () use (&$seen) { return count($seen) === 1; });
assertTrue(str_starts_with($head, 'HTTP/1.1 401'), "an int is that status: $head");

// 4. a subprotocol the client offered is chosen
$mode = 'subprotocol';
$seen = [];
$head = upgrade($server, $port, "Sec-WebSocket-Protocol: chat, superchat\r\n", function () use (&$seen) { return count($seen) === 1; });
assertTrue(str_starts_with($head, 'HTTP/1.1 101') && stripos($head, 'Sec-WebSocket-Protocol: superchat') !== false, "subprotocol: $head");
assertSame(['chat', 'superchat'], $seen[0]->offered, 'offered');

// 5. the handler answers later, through the Upgrade it kept
$mode = 'later';
$seen = [];
$pendingUpgrade = null;
$pending = ws_request_async($port, "Cookie: session=good\r\n");
pump($server, function () use (&$pendingUpgrade) { return $pendingUpgrade !== null; });
assertTrue(!$pendingUpgrade->answered(), 'not answered yet');
$pendingUpgrade->reject(429);
assertTrue($pendingUpgrade->answered(), 'answered');
assertThrows(\LogicException::class, fn () => $pendingUpgrade->reject(403), 'answer once');
usleep(100_000);
[, $head] = ws_finish($pending);
assertTrue(str_starts_with($head, 'HTTP/1.1 429'), "later: $head");

// 6. FAIL CLOSED: a handler that throws refuses with 500 and the exception surfaces
$mode = 'throw';
$pending = ws_request_async($port, "Cookie: session=good\r\n");
$e = assertThrows(RuntimeException::class, function () use ($server) {
    $deadline = microtime(true) + 5;
    while (microtime(true) < $deadline) {
        $server->step(50);
    }
}, 'the exception leaves step()');
assertSame('login service is down', $e->getMessage());
usleep(100_000);
[, $head] = ws_finish($pending);
assertTrue(str_starts_with($head, 'HTTP/1.1 500'), "throw is 500, never accept: $head");

// 7. FAIL CLOSED: an unusable return value is 500 and UnexpectedValueException
$mode = 'garbage';
$pending = ws_request_async($port, "Cookie: session=good\r\n");
assertThrows(\UnexpectedValueException::class, function () use ($server) {
    $deadline = microtime(true) + 5;
    while (microtime(true) < $deadline) {
        $server->step(50);
    }
}, 'an unusable return');
usleep(100_000);
[, $head] = ws_finish($pending);
assertTrue(str_starts_with($head, 'HTTP/1.1 500'), "garbage is 500: $head");
$server->close();

// 8. the configuration must agree with the handlers: a hook nobody answers, or a handler nobody asks
$e = assertThrows(WsrvException::class, fn () => (new Server(['handshake_hook' => true], $library))->start(), 'hook without handler');
assertTrue(str_contains($e->getMessage(), 'authenticate'), 'names the handler: ' . $e->getMessage());
$lonely = new Server(['listen_port' => 0], $library);
$lonely->on('authenticate', fn (Upgrade $u) => true);
$e = assertThrows(WsrvException::class, fn () => $lonely->start(), 'handler without hook');
assertTrue(str_contains($e->getMessage(), 'handshake_hook'), 'names the setting: ' . $e->getMessage());

echo "embedded handshake interop: ok\n";
