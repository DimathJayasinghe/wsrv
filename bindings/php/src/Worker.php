<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

use Wsrv\Ipc\Event;
use Wsrv\Ipc\EventKind;
use Wsrv\Ipc\Pong;
use Wsrv\Ipc\StatusReply;

/**
 * The callback style on top of Client: register handlers, call run().
 *
 *     $worker = new Worker(Client::connect('/run/wsrv/ipc.sock'));
 *     $worker->on('message', fn (Connection $c, string $data, bool $binary) => $c->send($data));
 *     $worker->run();
 *
 * Handlers:
 *   open    fn(Connection $c, string $peerAddress)
 *   message fn(Connection $c, string $data, bool $binary)
 *   close   fn(Connection $c, int $code, string $reason, bool $clean)   the last event for $c
 *   error   fn(Connection $c, string $name)                            best effort, may be missing
 *   status  fn(int $request, int $status)   the answer to a command; StatusCode::OK means accepted
 *
 * Handlers run on the calling thread, one event at a time, in the order the daemon sent them. An
 * exception from a handler leaves run() (it is not swallowed); the connection to the daemon stays
 * usable, so the caller can log it and call run() again. A handler that takes long makes the
 * daemon slow the browsers down (backpressure), and past `ipc_stall_ms` of not reading at all the
 * daemon drops this worker, so keep handlers short or hand work to another process.
 */
final class Worker
{
    private const HANDLERS = ['open', 'message', 'close', 'error', 'status'];

    /** @var array<string, callable> */
    private array $handlers = [];
    /** @var array<int, Connection> connections between their open and close events */
    private array $connections = [];
    private bool $stopping = false;

    public function __construct(private readonly Client $client)
    {
    }

    public function on(string $event, callable $handler): self
    {
        if (!in_array($event, self::HANDLERS, true)) {
            throw new \InvalidArgumentException(
                "unknown event '$event'; expected one of " . implode(', ', self::HANDLERS)
            );
        }
        $this->handlers[$event] = $handler;
        return $this;
    }

    /** Connections currently open (as far as this worker has been told). */
    public function connectionCount(): int
    {
        return count($this->connections);
    }

    /**
     * Dispatch events until stop() is called.
     *
     * @throws ConnectionClosed when the daemon goes away
     */
    public function run(float $pollInterval = 1.0): void
    {
        $this->stopping = false;
        while (!$this->stopping) {
            $this->step($pollInterval);
        }
    }

    /** Ask run() to return after the handler that is running now. Safe to call from a handler. */
    public function stop(): void
    {
        $this->stopping = true;
    }

    /**
     * Wait up to $timeout seconds for one message and dispatch it.
     * For embedding in another loop: call it whenever the daemon socket is readable.
     *
     * @return bool true if a message was dispatched
     */
    public function step(float $timeout = 0.0): bool
    {
        $message = $this->client->poll($timeout);
        if ($message === null || $message instanceof Pong) {
            return $message !== null;
        }
        if ($message instanceof StatusReply) {
            $this->call('status', $message->request, $message->status);
            return true;
        }
        $this->dispatch($message);
        return true;
    }

    private function dispatch(Event $event): void
    {
        $conn = $this->connections[$event->conn] ?? null;
        switch ($event->kind) {
            case EventKind::Open:
                $conn = new Connection($this->client, $event->conn, $event->data);
                $this->connections[$event->conn] = $conn;
                $this->call('open', $conn, $event->data);
                return;
            case EventKind::Text:
            case EventKind::Binary:
                $this->call('message', $conn ?? $this->orphan($event), $event->data, $event->kind === EventKind::Binary);
                return;
            case EventKind::Close:
                // Forget the connection first: the handler may throw, and the id is dead either way.
                unset($this->connections[$event->conn]);
                $this->call('close', $conn ?? $this->orphan($event), $event->closeCode, $event->data, $event->clean);
                return;
            case EventKind::Error:
                $this->call('error', $conn ?? $this->orphan($event), $event->data);
                return;
        }
    }

    /** An event for a connection we saw no Open for (the daemon does not do this; stay safe). */
    private function orphan(Event $event): Connection
    {
        return new Connection($this->client, $event->conn);
    }

    private function call(string $name, mixed ...$args): void
    {
        if (isset($this->handlers[$name])) {
            ($this->handlers[$name])(...$args);
        }
    }
}
