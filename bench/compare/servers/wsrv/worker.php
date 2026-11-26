<?php
// The benchmark application on wsrv: the same three behaviours as every contender (see ../../README.md).
//   every connection joins the room "bench" when it opens
//   "bcast:<text>"  is published to the room (fan-out)
//   anything else   is echoed back
declare(strict_types=1);

// The package source is mounted read-only: the benchmark measures this checkout, not a published version.
spl_autoload_register(static function (string $class): void {
    if (strncmp($class, 'Wsrv\\', 5) === 0) {
        $file = '/wsrv-src/' . str_replace('\\', '/', substr($class, 5)) . '.php';
        if (is_file($file)) {
            require $file;
        }
    }
});

use Wsrv\{Client, Connection, Worker};

$worker = new Worker(Client::connect(getenv('WSRV_SOCKET') ?: '/run/wsrv/ipc.sock'));
$worker->on('open', function (Connection $c): void {
    $c->subscribe('bench');
});
$worker->on('message', function (Connection $c, string $data, bool $binary) use ($worker): void {
    if (str_starts_with($data, 'bcast:')) {
        $worker->publish('bench', substr($data, 6));
    } else {
        $c->send($data);
    }
});
$worker->run();
