<?php

use Illuminate\Foundation\Application;
use Illuminate\Foundation\Configuration\Exceptions;
use Illuminate\Foundation\Configuration\Middleware;

// The package is loaded from its source directory, as if composer had installed it (run.sh fills in the path).
spl_autoload_register(static function (string $class): void {
    if (str_starts_with($class, 'Wsrv\\')) {
        $file = '__PKG_SRC__/' . str_replace('\\', '/', substr($class, 5)) . '.php';
        is_file($file) && require $file;
    }
});

return Application::configure(basePath: dirname(__DIR__))
    ->withProviders([Wsrv\Laravel\WsrvServiceProvider::class])   // what package discovery does
    ->withRouting(web: __DIR__.'/../routes/web.php', commands: __DIR__.'/../routes/console.php')
    // The broadcasting auth route, behind the session, CSRF and auth middleware of a real application.
    ->withBroadcasting(__DIR__.'/../routes/channels.php', ['middleware' => ['web', 'auth']])
    ->withMiddleware(function (Middleware $middleware): void {
    })
    ->withExceptions(function (Exceptions $exceptions): void {
    })->create();
