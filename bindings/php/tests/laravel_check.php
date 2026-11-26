<?php
// SPDX-License-Identifier: Apache-2.0
// The wsrv Laravel glue against a REAL Laravel application: the `wsrv` broadcasting driver, the
// /broadcasting/auth route, Broadcast::channel() callbacks, broadcast()->toOthers() and encrypted
// channels. CI does not run it (it needs a Laravel application with vendor/ installed); run it
// by hand, see docs/LARAVEL.md "Testing the glue", e.g. with any Laravel 11+ application:
//
//   php tests/laravel_check.php /path/to/laravel-app
//
// Nothing here talks to a daemon: the publish endpoint is a recording stand-in, and the Pusher
// protocol itself is covered by pusher_interop.php.

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

use Illuminate\Contracts\Console\Kernel;
use Illuminate\Contracts\Http\Kernel as HttpKernel;
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Auth;
use Illuminate\Support\Facades\Broadcast;
use Wsrv\Laravel\WsrvBroadcaster;
use Wsrv\Pusher\EncryptedChannels;
use Wsrv\Pusher\Signature;

$appDir = $argv[1] ?? fail('usage: php tests/laravel_check.php /path/to/laravel-app');
$loader = require $appDir . '/vendor/autoload.php';
$loader->addPsr4('Wsrv\\', __DIR__ . '/../src/');
$app = require $appDir . '/bootstrap/app.php';
$app->make(Kernel::class)->bootstrap();

const APP_KEY = 'laravel-check-key';
const APP_SECRET = 'laravel-check-secret-0123456789ab';
$masterKey = base64_encode(random_bytes(32));
$published = [];   // [topic, payload, except] as the daemon's publish endpoint saw them

// The Publisher is final, and takes a transport for tests: it answers like a daemon that delivered to 1.
$transport = static function (string $host, string $request, int $port, float $timeout) use (&$published): string {
    preg_match('#^POST /publish\?topic=([^&\s]+)(?:&except=(\d+))? HTTP#', $request, $m);
    $published[] = [$m[1], substr($request, strpos($request, "\r\n\r\n") + 4), (int) ($m[2] ?? 0)];
    $body = "subscribers=1 accepted=1 refused=0\n";
    return "HTTP/1.1 200 OK\r\nContent-Length: " . strlen($body) . "\r\n\r\n" . $body;
};

config(['broadcasting.default' => 'wsrv', 'broadcasting.connections.wsrv' => [
    'driver' => 'wsrv', 'key' => APP_KEY, 'secret' => APP_SECRET,
    'publish_url' => 'http://127.0.0.1:9200', 'publish_token' => 'tok-' . str_repeat('x', 40),
    'encryption_master_key_base64' => $masterKey,
]]);
// Register the provider by hand, as package discovery would, and swap in the recording transport.
$app->register(Wsrv\Laravel\WsrvServiceProvider::class);
$broadcaster = null;
$app->make('Illuminate\Broadcasting\BroadcastManager')->extend('wsrv', function ($app, array $config) use (&$broadcaster, $transport) {
    $connection = Wsrv\Laravel\ConnectionConfig::fromArray($config);
    $publisher = new Wsrv\Publisher('http://127.0.0.1:9200', 'tok-' . str_repeat('x', 40), 2.0, $transport);
    return $broadcaster = new WsrvBroadcaster($connection->gateway(), $publisher, $connection->encryption());
});

class CheckUser extends Illuminate\Foundation\Auth\User
{
    public $id = 7;
    public function getAuthIdentifier() { return $this->id; }
}

Broadcast::channel('chat.{id}', fn ($user, $id) => (int) $user->id === (int) $id);
Broadcast::channel('room.{id}', fn ($user, $id) => ['name' => 'Ada', 'id' => $user->id]);
Broadcast::channel('secret.{id}', fn ($user, $id) => true);
Broadcast::channel('locked', fn () => false);
Broadcast::routes(['middleware' => []]);
Broadcast::driver('wsrv');   // builds the broadcaster through the driver

/** POST /broadcasting/auth through the real HTTP kernel, as Echo does; [status, decoded body]. */
$auth = function (string $channel, string $socket, ?int $asUser = 7) use ($app): array {
    if ($asUser !== null) {
        $user = new CheckUser();
        $user->id = $asUser;
        Auth::guard()->setUser($user);
    } else {
        Auth::forgetGuards();
    }
    $request = Request::create('/broadcasting/auth', 'POST', ['channel_name' => $channel, 'socket_id' => $socket]);
    $response = $app->make(HttpKernel::class)->handle($request);
    return [$response->getStatusCode(), json_decode($response->getContent(), true)];
};

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

$sock = '41.123456';

$check('the wsrv driver is built through BroadcastManager', function () use (&$broadcaster) {
    assertTrue($broadcaster instanceof WsrvBroadcaster, 'not built');
});

$check('php artisan wsrv:work is registered', function () use ($app) {
    $commands = $app->make(Kernel::class)->all();
    assertTrue(isset($commands['wsrv:work']), 'wsrv:work is not an artisan command');
});

$check('private channel: the callback decides, the answer is the signature the gateway verifies', function () use ($auth, $sock) {
    [$status, $body] = $auth('private-chat.7', $sock);
    assertSame(200, $status);
    assertSame(['auth' => Signature::authorize(APP_KEY, APP_SECRET, $sock, 'private-chat.7')], $body);
    [$status] = $auth('private-chat.8', $sock);
    assertSame(403, $status);
    [$status] = $auth('private-locked', $sock);
    assertSame(403, $status);
});

$check('no user: a private channel is 403', function () use ($auth, $sock) {
    [$status] = $auth('private-chat.7', $sock, null);
    assertSame(403, $status);
});

$check('presence channel: channel_data carries the user id and the callback info, signed together', function () use ($auth, $sock) {
    [$status, $body] = $auth('presence-room.1', $sock);
    assertSame(200, $status);
    $data = json_decode($body['channel_data'], true);
    assertSame(['user_id' => '7', 'user_info' => ['name' => 'Ada', 'id' => 7]], $data);
    assertSame(Signature::authorize(APP_KEY, APP_SECRET, $sock, 'presence-room.1', $body['channel_data']), $body['auth']);
});

$check('a socket id or channel name the daemon could not have made is refused, without a signature', function () use ($auth) {
    foreach ([['private-chat.7', 'nope'], ['private-chat.7', '41.1;x'], ['private-chat=7', '41.1'], ['private-cache-x', '41.1']] as [$channel, $socket]) {
        [$status, $body] = $auth($channel, $socket);
        assertSame(403, $status, "$channel $socket");
        assertTrue(!isset($body['auth']), 'a signature was given');
    }
});

$check('encrypted channel: the answer carries the shared secret pusher-js derives', function () use ($auth, $sock, $masterKey) {
    [$status, $body] = $auth('private-encrypted-secret.1', $sock);
    assertSame(200, $status);
    $crypto = EncryptedChannels::fromBase64($masterKey);
    assertSame($crypto->sharedSecretBase64('private-encrypted-secret.1'), $body['shared_secret']);
    assertSame(Signature::authorize(APP_KEY, APP_SECRET, $sock, 'private-encrypted-secret.1'), $body['auth']);
});

class Shipped implements Illuminate\Contracts\Broadcasting\ShouldBroadcastNow
{
    use Illuminate\Broadcasting\InteractsWithSockets;
    public function __construct(public array $order, private string $channel = 'orders') {}
    public function broadcastOn(): array { return [new Illuminate\Broadcasting\Channel($this->channel)]; }
    public function broadcastAs(): string { return 'order.shipped'; }
}

$check('broadcast(): the event reaches the daemon as the frame pusher-js expects', function () use (&$published) {
    $published = [];
    broadcast(new Shipped(['id' => 7]));
    assertSame(1, count($published));
    [$topic, $payload, $except] = $published[0];
    assertSame('orders', $topic);
    assertSame(0, $except);
    $frame = json_decode($payload, true);
    assertSame('order.shipped', $frame['event']);
    assertSame('orders', $frame['channel']);
    assertTrue(is_string($frame['data']), 'data is a JSON string');
    $data = json_decode($frame['data'], true);
    assertSame(['id' => 7], $data['order']);
    assertTrue(!isset($data['socket']), 'the socket id is not sent to the browsers');
});

$check('toOthers(): the X-Socket-ID header becomes the connection left out', function () use ($app, &$published) {
    $published = [];
    $request = Request::create('/x', 'POST');
    $request->headers->set('X-Socket-ID', '41.123456');
    $app->instance('request', $request);
    broadcast(new Shipped(['id' => 8]))->toOthers();
    assertSame(1, count($published));
    assertSame(41, $published[0][2]);
    assertTrue(!str_contains($published[0][1], '123456'), 'the socket id leaked into the payload');
});

$check('toOthers() with a socket id this daemon cannot have made delivers to everyone', function () use ($app, &$published) {
    $published = [];
    $request = Request::create('/x', 'POST');
    $request->headers->set('X-Socket-ID', 'garbage');
    $app->instance('request', $request);
    broadcast(new Shipped(['id' => 9]))->toOthers();
    assertSame(0, $published[0][2]);
});

$check('a private encrypted channel is encrypted: the daemon never sees the payload', function () use (&$published, $masterKey) {
    $published = [];
    broadcast(new Shipped(['id' => 10, 'note' => 'confidential'], 'private-encrypted-secret.1'));
    [, $payload] = $published[0];
    assertTrue(!str_contains($payload, 'confidential'), 'plaintext reached the daemon');
    $frame = json_decode($payload, true);
    $box = json_decode($frame['data'], true);
    $plain = EncryptedChannels::fromBase64($masterKey)->decrypt('private-encrypted-secret.1', $box['nonce'], $box['ciphertext']);
    assertSame(['id' => 10, 'note' => 'confidential'], json_decode($plain, true)['order'] ?? json_decode($plain, true));
});

$check('a name the daemon would refuse fails loudly instead of being rewritten', function () use (&$published) {
    $published = [];
    $e = assertThrows(Illuminate\Broadcasting\BroadcastException::class, fn () => broadcast(new Shipped(['id' => 1], 'orders=1')));
    assertTrue(str_contains($e->getMessage(), 'orders=1'));
    assertSame([], $published);
});

echo count($failures) === 0 ? "all laravel checks passed\n" : count($failures) . " laravel check(s) failed\n";
exit($failures === [] ? 0 : 1);
