<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Laravel;

use Illuminate\Broadcasting\BroadcastManager;
use Illuminate\Contracts\Foundation\Application;
use Illuminate\Support\ServiceProvider;

/**
 * Registers the `wsrv` broadcasting driver and the `wsrv:work` command. Laravel finds this class
 * through composer's package discovery (the `extra.laravel` key of wsrv/client); with discovery
 * turned off, add `Wsrv\Laravel\WsrvServiceProvider::class` to bootstrap/providers.php.
 */
final class WsrvServiceProvider extends ServiceProvider
{
    public function register(): void
    {
        $this->callAfterResolving(BroadcastManager::class, function (BroadcastManager $manager): void {
            $manager->extend('wsrv', function (Application $app, array $config): WsrvBroadcaster {
                $connection = ConnectionConfig::fromArray($config);
                return new WsrvBroadcaster($connection->gateway(), $connection->publisher(), $connection->encryption());
            });
        });
    }

    public function boot(): void
    {
        if ($this->app->runningInConsole()) {
            $this->commands([WorkCommand::class]);
        }
    }
}
