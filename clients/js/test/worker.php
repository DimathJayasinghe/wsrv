<?php
// SPDX-License-Identifier: Apache-2.0
// The PHP side of the JS interop test: a supervised worker that echoes, and joins rooms on "join <room>".
declare(strict_types=1);

spl_autoload_register(static function (string $class): void {
    if (strncmp($class, 'Wsrv\\', 5) !== 0) {
        return;
    }
    $file = __DIR__ . '/../../../bindings/php/src/' . str_replace('\\', '/', substr($class, 5)) . '.php';
    if (is_file($file)) {
        require $file;
    }
});

use Wsrv\{Connection, Supervisor, Worker};

$supervisor = Supervisor::forSocket($argv[1]);
$supervisor->installSignalHandlers();
$supervisor->run(function (Worker $worker): void {
    $worker->on('message', function (Connection $c, string $data): void {
        if (str_starts_with($data, 'join ')) {
            $c->subscribe(substr($data, 5));
            $c->send('joined ' . substr($data, 5));
        } else {
            $c->send('echo ' . $data);
        }
    });
});
