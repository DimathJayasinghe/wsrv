<?php
// SPDX-License-Identifier: Apache-2.0
// The stand-in for a Laravel app in the browser run (tests/echo_browser/run.php): serves the test page
// and the two JavaScript bundles, answers /broadcasting/auth the way WsrvBroadcaster does, and
// publishes on request the way WsrvBroadcaster::broadcast() does. Started by run.php with `php -S`.

declare(strict_types=1);

spl_autoload_register(static function (string $class): void {
    if (str_starts_with($class, 'Wsrv\\')) {
        $file = __DIR__ . '/../../src/' . str_replace('\\', '/', substr($class, 5)) . '.php';
        is_file($file) && require $file;
    }
});

use Wsrv\Publisher;
use Wsrv\Pusher\ChannelKind;
use Wsrv\Pusher\ChannelName;
use Wsrv\Pusher\EncryptedChannels;
use Wsrv\Pusher\Frame;
use Wsrv\Pusher\GatewayConfig;
use Wsrv\Pusher\SocketId;

$config = new GatewayConfig((string) getenv('WSRV_KEY'), (string) getenv('WSRV_SECRET'));
$encryption = EncryptedChannels::fromBase64((string) getenv('WSRV_ENC_KEY'));
$path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
$method = $_SERVER['REQUEST_METHOD'];

function json_out(int $status, array $body): void
{
    http_response_code($status);
    header('Content-Type: application/json');
    echo json_encode($body, JSON_THROW_ON_ERROR);
}

if ($method === 'GET' && $path === '/') {
    $html = (string) file_get_contents(__DIR__ . '/index.html');
    header('Content-Type: text/html; charset=utf-8');
    echo str_replace(['__KEY__', '__WS_PORT__', '__CSRF__'], [$config->appKey, (string) getenv('WSRV_DAEMON_PORT'), 'none'], $html);
    return;
}
if ($method === 'GET' && $path === '/vendor/pusher.js') {
    header('Content-Type: text/javascript');
    readfile((string) getenv('WSRV_PUSHER_JS'));
    return;
}
if ($method === 'GET' && $path === '/vendor/echo.js') {
    header('Content-Type: text/javascript');
    readfile((string) getenv('WSRV_ECHO_JS'));
    return;
}
// pusher-js posts the auth request form-encoded; the page's own calls are JSON.
$body = json_decode((string) file_get_contents('php://input'), true);
if (!is_array($body)) {
    $body = $_POST;
}

// What Laravel's /broadcasting/auth does: the user is whoever is signed in (here: the X-User header).
if ($method === 'POST' && $path === '/broadcasting/auth') {
    $users = ['ada' => ['id' => '1', 'name' => 'Ada'], 'bob' => ['id' => '2', 'name' => 'Bob']];
    $user = $users[$_SERVER['HTTP_X_USER'] ?? ''] ?? null;
    $channel = $body['channel_name'] ?? null;
    $socket = $body['socket_id'] ?? null;
    if ($user === null || !is_string($channel) || !is_string($socket) || SocketId::connection($socket) === null
        || ChannelName::problem($channel) !== null || str_contains($channel, 'forbidden')) {
        json_out(403, ['message' => 'denied']);
        return;
    }
    $response = [];
    if (ChannelName::kind($channel) === ChannelKind::Presence) {
        $data = json_encode(['user_id' => $user['id'], 'user_info' => ['name' => $user['name']]], JSON_THROW_ON_ERROR);
        $response = ['auth' => $config->authorize($socket, $channel, $data), 'channel_data' => $data];
    } else {
        $response = ['auth' => $config->authorize($socket, $channel)];
    }
    if (EncryptedChannels::isEncrypted($channel)) {
        $response['shared_secret'] = $encryption->sharedSecretBase64($channel);
    }
    json_out(200, $response);
    return;
}

// What broadcast(...)->toOthers() does: publish a frame to the daemon, leaving the sender's socket out.
if ($method === 'POST' && $path === '/emit') {
    $channel = (string) ($body['channel'] ?? '');
    $data = $body['data'] ?? [];
    $payload = EncryptedChannels::isEncrypted($channel)
        ? $encryption->encrypt($channel, json_encode($data, JSON_THROW_ON_ERROR))
        : $data;
    $frame = Frame::encode((string) $body['event'], $payload, $channel);
    $except = is_string($body['socket'] ?? null) ? (SocketId::connection($body['socket']) ?? 0) : 0;
    $publisher = new Publisher((string) getenv('WSRV_PUBLISH_URL'), trim((string) file_get_contents((string) getenv('WSRV_TOKEN_FILE'))));
    $result = $publisher->publish($channel, $frame, false, $except);
    json_out(200, ['subscribers' => $result->subscribers]);
    return;
}

// The page asks run.php to restart the daemon and the worker under the connected browsers.
if ($method === 'POST' && $path === '/restart') {
    touch((string) getenv('WSRV_RESTART_FLAG'));
    json_out(200, ['ok' => true]);
    return;
}
if ($method === 'GET' && $path === '/restarted') {
    json_out(200, ['done' => is_file((string) getenv('WSRV_RESTARTED_FLAG'))]);
    return;
}

if ($method === 'POST' && $path === '/result') {
    file_put_contents((string) getenv('WSRV_RESULT_FILE'), json_encode($body));
    json_out(200, ['ok' => true]);
    return;
}
json_out(404, ['message' => 'not found']);
