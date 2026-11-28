<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Laravel;

use Illuminate\Console\Command;
use Wsrv\Supervisor;
use Wsrv\Worker;
use Wsrv\Pusher\Gateway;

/**
 * `php artisan wsrv:echo`: the long-running process that speaks the Pusher protocol to the browsers
 * through wsrvd (ADR-0042). Run one per daemon, under systemd, Supervisor or a container restart
 * policy. It reconnects by itself when the daemon restarts; the browsers' sockets are closed by the
 * daemon in that moment and Echo reconnects and resubscribes.
 */
final class EchoCommand extends Command
{
    protected $signature = 'wsrv:echo
        {--connection=wsrv : the name of the broadcasting connection to serve}
        {--stats-interval=60 : seconds between lines of statistics on stderr (0 turns them off)}';

    protected $description = 'Serve the Pusher protocol for Laravel Echo through wsrvd';

    public function handle(): int
    {
        $name = (string) $this->option('connection');
        $config = config("broadcasting.connections.$name");
        if (!is_array($config) || ($config['driver'] ?? null) !== 'wsrv') {
            $this->components->error("broadcasting.connections.$name is not a wsrv connection");
            return self::FAILURE;
        }
        try {
            $connection = ConnectionConfig::fromArray($config);
            $gatewayConfig = $connection->gateway();
            $supervisor = Supervisor::forSocket($connection->ipcEndpoint(), $connection->ipcToken());
            $checkAppKey = $connection->checkAppKey();
            $statsInterval = ConnectionConfig::statsInterval($this->option('stats-interval'));
        } catch (\InvalidArgumentException $e) {
            $this->components->error($e->getMessage());
            return self::FAILURE;
        }
        $supervisor->installSignalHandlers();
        $this->components->info('wsrv:echo serving "' . $name . '" through ' . $connection->ipcEndpoint());
        // A new Gateway for every connection to the daemon: when the daemon restarts it closes every
        // browser, so no presence state may outlive the link. Its counters start again with it.
        $supervisor->run(static function (Worker $worker) use ($gatewayConfig, $checkAppKey, $statsInterval): void {
            $gateway = Gateway::forWorker($gatewayConfig, $worker, $checkAppKey);
            if ($statsInterval > 0) {
                // Numbers only (GatewayStats), so nothing a browser sent can end up in the line.
                $worker->every((float) $statsInterval, static function () use ($gateway): void {
                    fwrite(STDERR, '[wsrv] gateway ' . $gateway->stats()->line() . "\n");
                });
            }
        });
        return self::SUCCESS;
    }
}
