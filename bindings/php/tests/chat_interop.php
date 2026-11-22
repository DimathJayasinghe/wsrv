<?php
// SPDX-License-Identifier: Apache-2.0
// End to end: the PHP chat example (examples/chat/Chat.php) on the REAL libwsrv.so through the embedded
// binding, with two raw WebSocket clients. One PHP thread: a client sends, the server steps until the
// command was handled, the client then reads what it was sent.
// Run with: php tests/chat_interop.php /path/to/libwsrv.so

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';
require __DIR__ . '/ws_client.php';
require __DIR__ . '/../examples/chat/Chat.php';

use Wsrv\Embedded\Connection;
use Wsrv\Embedded\Server;
use Wsrv\Examples\Chat;

$library = $argv[1] ?? fail('usage: php tests/chat_interop.php /path/to/libwsrv.so');
extension_loaded('FFI') || fail('ext-ffi is not loaded: the embedded binding needs it');

$chat = new Chat();
$handled = 0;
$closed = 0;
$ids = [];
$server = new Server(['listen_port' => 0, 'allow_origin' => ['https://app.example.com']], $library);
$server->on('open', function (Connection $c, string $peer) use (&$ids) {
    $ids[] = $c->id;
});
$server->on('message', function (Connection $c, string $data, bool $binary) use ($chat, &$handled) {
    $chat->onMessage($c, $data, $binary);
    $handled++;
});
$server->on('close', function (Connection $c, int $code, string $reason, bool $clean) use ($chat, &$closed) {
    $chat->onClose($c);
    $closed++;
});
$server->start();
$port = $server->port();

// Closures capture by reference (`use (&$x)`): an arrow function would freeze the value.
function waitFor(Server $server, callable $done): void
{
    $deadline = microtime(true) + 5.0;
    while (!$done()) {
        microtime(true) < $deadline || fail('timed out waiting for the server');
        $server->step(50);
    }
}

/** Send one command as a text message and wait until the server handled it. */
function command(Server $server, $client, string $text, int &$handled): void
{
    $before = $handled;
    ws_send($client, 1, $text);
    waitFor($server, function () use (&$handled, $before) { return $handled > $before; });
}

/** The next text message the client received. */
function receive($client): string
{
    [$opcode, $payload] = ws_read($client);
    assertSame(1, $opcode, 'a text message');
    return $payload;
}

/** A command and its one reply. */
function ask(Server $server, $client, string $text, int &$handled): string
{
    command($server, $client, $text, $handled);
    return receive($client);
}

$a = ws_connect($port);
$b = ws_connect($port);
waitFor($server, function () use (&$ids) { return count($ids) === 2; });
[$idA, $idB] = $ids;

// 1. joining, speaking, the room's members
assertSame('ok joined lobby', ask($server, $a, 'join lobby', $handled), 'A joins');
assertSame('ok joined lobby', ask($server, $b, 'join lobby', $handled), 'B joins');
assertSame('ok sent lobby 1 0', ask($server, $a, 'say lobby hello there', $handled), 'the sender is told how many were reached');
assertSame("msg lobby user$idA: hello there", receive($b), 'the other member hears it');
assertSame('ok members lobby 2', ask($server, $a, 'members lobby', $handled));
assertSame('ok rooms lobby', ask($server, $a, 'rooms', $handled));

// 2. policy: speak and ask only where you joined; the sender does not hear itself
assertSame('error: not in other', ask($server, $a, 'say other hi', $handled), 'a stranger cannot speak');
assertSame('error: not in other', ask($server, $a, 'members other', $handled), 'a stranger cannot probe rooms');
assertSame('error: already in lobby', ask($server, $a, 'join lobby', $handled));

// 3. every input bound is the example's own and is enforced
assertTrue(str_starts_with(ask($server, $a, 'join bad/name', $handled), 'error: a room name'), 'characters');
assertTrue(str_starts_with(ask($server, $a, 'join ' . str_repeat('x', Chat::MAX_ROOM_NAME + 1), $handled), 'error: a room name'), 'length');
assertSame('error: expected one room name', ask($server, $a, 'join', $handled), 'no room');
assertSame('error: expected one room name', ask($server, $a, 'join a b', $handled), 'two words');
assertTrue(str_starts_with(ask($server, $a, 'say lobby ' . str_repeat('y', Chat::MAX_TEXT + 1), $handled), 'error: message too long'), 'text length');
for ($i = 2; $i <= Chat::MAX_ROOMS_PER_CONNECTION; $i++) {
    assertSame("ok joined r$i", ask($server, $a, "join r$i", $handled), "room $i");
}
assertTrue(str_starts_with(ask($server, $a, 'join one-too-many', $handled), 'error: too many rooms'), 'rooms per connection');
assertSame('error: unknown command (join, leave, say, members, rooms)', ask($server, $a, 'dance', $handled));
$before = $handled;
ws_send($a, 2, 'binary');
waitFor($server, function () use (&$handled, $before) { return $handled > $before; });
assertSame('error: text commands only', receive($a), 'binary is refused');

// 4. leaving, and a connection that just disappears gives its rooms back
assertSame('ok left lobby', ask($server, $a, 'leave lobby', $handled));
assertSame('ok members lobby 1', ask($server, $b, 'members lobby', $handled), 'only B is left');
$roomsBefore = $chat->roomCount();
ws_send($a, 8, pack('n', 1000));
fclose($a);
waitFor($server, function () use (&$closed) { return $closed === 1; });
assertSame(1, $chat->roomCount(), 'A\'s 15 other rooms are gone; only B\'s lobby is left (was ' . $roomsBefore . ')');
fclose($b);
waitFor($server, function () use (&$closed) { return $closed === 2; });
assertSame(0, $chat->roomCount(), 'no connections, no rooms');

$server->close();
echo "chat interop: ok\n";
