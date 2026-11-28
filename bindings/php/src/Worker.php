<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

use Wsrv\Ipc\Codec;
use Wsrv\Ipc\Event;
use Wsrv\Ipc\EventKind;
use Wsrv\Ipc\HandshakeRequest;
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
 *   authenticate fn(Upgrade $u): bool|int|string|null   who may connect (needs `handshake_hook` on the
 *           daemon; see "Authenticating" below)
 *
 * Authenticating. With the daemon's `handshake_hook = true`, no browser gets a WebSocket until this
 * worker says so. The handler sees the request (`$u->request`: path, query, Origin, offered
 * subprotocols, the forwarded headers, `cookie($name)`) and decides by RETURNING:
 *   true      accept            false  reject with 403         int  reject with that status
 *   string    accept, choosing that subprotocol (it must be one the client offered)
 *   null      "later": answer yourself through `$u->accept()` / `$u->reject()` within the daemon's
 *             decision timeout (default 3 s); a browser that gets no answer is told 503
 * Fail closed: no handler registered, a handler that throws (500, then the exception surfaces) and
 * an unusable return value (500, UnexpectedValueException) all REFUSE the upgrade, never accept.
 * Keep the handler fast: a slow answer is a slow login. Look the session up in a cache.
 *
 * Handlers run on the calling thread, one event at a time, in the order the daemon sent them. An
 * exception from a handler leaves run() (it is not swallowed); the connection to the daemon stays
 * usable, so the caller can log it and call run() again. A handler that takes long makes the
 * daemon slow the browsers down (backpressure), and past `ipc_stall_ms` of not reading at all the
 * daemon drops this worker, so keep handlers short or hand work to another process.
 */
final class Worker
{
    private const HANDLERS = ['open', 'message', 'close', 'error', 'status', 'authenticate'];

    /** @var array<string, callable> */
    private array $handlers = [];
    /** @var array<int, Connection> connections between their open and close events */
    private array $connections = [];
    private bool $stopping = false;
    /** @var list<array{every: float, next: float, call: callable}> see every() */
    private array $timers = [];
    private readonly \Closure $clock;

    /** @param callable(): float|null $clock seconds as a float; for tests (default: a monotonic clock) */
    public function __construct(private readonly Client $client, ?callable $clock = null)
    {
        $this->clock = $clock !== null ? \Closure::fromCallable($clock) : static fn (): float => hrtime(true) / 1e9;
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

    /**
     * Send to every subscriber of $topic except connection $except (0 = nobody); see
     * Client::publish(). From another process, use Publisher over HTTP instead. @return int the request id
     */
    public function publish(string $topic, string $payload, bool $binary = false, int $except = 0): int
    {
        return $this->client->publish($topic, $payload, $binary, $except);
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
            if (!$this->stopping) {
                $this->runTimers();
            }
        }
    }

    /**
     * Call $fn about every $seconds while run() is serving (ADR-0042 A2): a statistics line, a sweep of
     * your own state. The time is checked after each step(), so a call is at most one poll interval late
     * (lower the argument of run() for a finer grain) and a long handler delays it. It does not catch up:
     * after a pause of ten intervals there is one call, not ten. An exception leaves run() like one from
     * a handler does, and the next call is already scheduled, so a timer that throws does not spin.
     * Timers belong to this Worker; under a Supervisor, register them in the callback that gets each new one.
     *
     * @param callable(): void $fn
     */
    public function every(float $seconds, callable $fn): self
    {
        if (!is_finite($seconds) || $seconds <= 0.0) {
            throw new \InvalidArgumentException('every() needs an interval above zero seconds');
        }
        $this->timers[] = ['every' => $seconds, 'next' => ($this->clock)() + $seconds, 'call' => $fn];
        return $this;
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

    private function runTimers(): void
    {
        if ($this->timers === []) {
            return;
        }
        $now = ($this->clock)();
        foreach ($this->timers as $i => $timer) {
            if ($now >= $timer['next']) {
                $this->timers[$i]['next'] = $now + $timer['every'];
                ($timer['call'])();
            }
        }
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
            case EventKind::Handshake:
                $this->authenticate($event);
                return;
        }
    }

    /** A browser's upgrade waits for us. Every path that does not accept, refuses. */
    private function authenticate(Event $event): void
    {
        try {
            $request = HandshakeRequest::parse($event->data);
        } catch (ProtocolError $e) {
            $this->refuseQuietly($event->conn, 503);  // a daemon bug, but do not leave the browser waiting
            throw $e;
        }
        $upgrade = new Upgrade($this->client, $event->conn, $request);
        if (!isset($this->handlers['authenticate'])) {
            $upgrade->reject(503);  // the daemon asks and nobody here can answer
            return;
        }
        try {
            $decision = ($this->handlers['authenticate'])($upgrade);
            if (!$upgrade->answered()) {
                $this->apply($upgrade, $decision);
            }
        } catch (\Throwable $e) {
            if (!$upgrade->answered()) {
                $this->refuseQuietly($event->conn, 500);
            }
            throw $e;
        }
    }

    private function apply(Upgrade $upgrade, mixed $decision): void
    {
        if ($decision === null) {
            return;  // the handler answers later, through $upgrade
        }
        if ($decision === true) {
            $upgrade->accept();
        } elseif ($decision === false) {
            $upgrade->reject(403);
        } elseif (is_int($decision) && in_array($decision, Codec::REJECT_STATUSES, true)) {
            $upgrade->reject($decision);
        } elseif (is_string($decision) && in_array($decision, $upgrade->request->offered, true)) {
            $upgrade->accept($decision);
        } else {
            throw new \UnexpectedValueException(
                'authenticate must return true, false, null, one of ' . implode(', ', Codec::REJECT_STATUSES)
                . ', or a subprotocol the client offered; got ' . get_debug_type($decision)
            );
        }
    }

    /** Best effort, for failure paths: the original problem is what the caller should see. */
    private function refuseQuietly(int $conn, int $status): void
    {
        try {
            $this->client->rejectHandshake($conn, $status);
        } catch (ConnectionClosed) {
            // the daemon is gone; it already refused every waiting upgrade
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
