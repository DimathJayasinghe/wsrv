<?php
// The soak worker: echo, and rooms. Runs under Supervisor, like a production worker.
declare(strict_types=1);

spl_autoload_register(static function (string $class): void {
    if (strncmp($class, 'Wsrv\\', 5) === 0) {
        $file = '/wsrv-src/' . str_replace('\\', '/', substr($class, 5)) . '.php';
        if (is_file($file)) {
            require $file;
        }
    }
});

use Wsrv\{Connection, Supervisor, Worker};

$supervisor = Supervisor::forSocket('/run/wsrv/ipc.sock');
$supervisor->installSignalHandlers();
$supervisor->run(function (Worker $worker): void {
    $worker->on('message', function (Connection $c, string $data) use ($worker): void {
        if (str_starts_with($data, 'join ')) {
            $c->subscribe(substr($data, 5));
        } elseif (str_starts_with($data, 'bcast ')) {
            $worker->publish('soak', substr($data, 6));
        } else {
            $c->send('echo ' . $data);
        }
    });
});
