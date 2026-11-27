<?php
// SPDX-License-Identifier: Apache-2.0
// The Pusher gateway the way `php artisan wsrv:echo` runs it (Supervisor, Gateway::forWorker, the statistics
// line every few seconds), without Laravel, for the load and churn runs (run.py).
//
//   php worker.php --socket=/run/wsrv/ipc.sock --key=K --secret=S [--stats-interval=5] [--options='{"maxStrikes":8}']
//
// --options is a JSON object of GatewayConfig constructor arguments by name (camelCase). SIGTERM stops it and
// prints the last statistics line as "gateway final ...".

declare(strict_types=1);

spl_autoload_register(static function (string $class): void {
    if (str_starts_with($class, 'Wsrv\\')) {
        $file = __DIR__ . '/../../src/' . str_replace('\\', '/', substr($class, 5)) . '.php';
        is_file($file) && require $file;
    }
});

use Wsrv\Pusher\Gateway;
use Wsrv\Pusher\GatewayConfig;
use Wsrv\Supervisor;
use Wsrv\Worker;

$opt = getopt('', ['socket:', 'key:', 'secret:', 'stats-interval::', 'options::']);
foreach (['socket', 'key', 'secret'] as $required) {
    isset($opt[$required]) || (fwrite(STDERR, "worker.php: --$required is required\n") && exit(2));
}
$interval = (float) ($opt['stats-interval'] ?? 5);
$options = json_decode((string) ($opt['options'] ?? '{}'), true, 4, JSON_THROW_ON_ERROR);
is_array($options) || (fwrite(STDERR, "worker.php: --options must be a JSON object\n") && exit(2));

$config = new GatewayConfig((string) $opt['key'], (string) $opt['secret'], ...$options);
$supervisor = Supervisor::forSocket((string) $opt['socket']);
$supervisor->installSignalHandlers();

$gateway = null;
$supervisor->run(static function (Worker $worker) use ($config, $interval, &$gateway): void {
    $gateway = Gateway::forWorker($config, $worker);
    $current = $gateway;
    if ($interval > 0) {   // as wsrv:echo does: 0 turns the line off
        $worker->every($interval, static function () use ($current): void {
            fwrite(STDERR, '[wsrv] gateway ' . $current->stats()->line() . "\n");
        });
    }
});
if ($gateway !== null) {
    fwrite(STDERR, '[wsrv] gateway final ' . $gateway->stats()->line() . "\n");
}
