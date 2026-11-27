<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

use Wsrv\Connection;
use Wsrv\Worker;

/** The Link over a real Worker. It holds the Connection handles between their open and close events. */
final class WorkerLink implements Link
{
    /** @var array<int, Connection> */
    private array $connections = [];

    public function __construct(private readonly Worker $worker)
    {
    }

    public function remember(Connection $connection): void
    {
        $this->connections[$connection->id] = $connection;
    }

    public function forget(int $conn): void
    {
        unset($this->connections[$conn]);
    }

    public function send(int $conn, string $text): int
    {
        return ($this->connections[$conn] ?? null)?->send($text) ?? 0;
    }

    public function subscribe(int $conn, string $topic): int
    {
        return ($this->connections[$conn] ?? throw new \LogicException('no such connection'))->subscribe($topic);
    }

    public function unsubscribe(int $conn, string $topic): int
    {
        return ($this->connections[$conn] ?? throw new \LogicException('no such connection'))->unsubscribe($topic);
    }

    public function publish(string $topic, string $payload, int $except): void
    {
        $this->worker->publish($topic, $payload, false, $except);
    }

    public function close(int $conn, int $code, string $reason): void
    {
        ($this->connections[$conn] ?? null)?->close($code, $reason);
    }
}
