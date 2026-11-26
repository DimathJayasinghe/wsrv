<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * Keeps a Worker running across daemon restarts (docs/DEPLOYMENT.md section 4).
 *
 * `Worker::run()` ends with ConnectionClosed when the daemon goes away, and a restarted daemon
 * accepts only one application: connecting again immediately can be refused (BACKPRESSURE) until the
 * daemon has noticed the old link is gone. This class is the loop every production worker needs:
 *
 *     Supervisor::forSocket('/run/wsrv/ipc.sock', token: $secret)->run(function (Worker $worker) {
 *         $worker->on('message', fn (Connection $c, string $data) => $c->send($data));
 *     });
 *
 * - The callback runs for every new connection to the daemon, with a fresh Worker: register all
 *   handlers there. State that must outlive a daemon restart belongs outside the callback, because
 *   the daemon closes the browsers' sockets when the application leaves, and they reconnect.
 * - Connection failures are retried with an exponentially growing, jittered delay (`minDelay` up to
 *   `maxDelay`). The delay starts over only after a session that lasted `resetAfter` seconds, so a
 *   daemon that accepts and dies at once is not hammered.
 * - Retried: nobody listening, a daemon that hangs up, BACKPRESSURE (the previous application's link
 *   is still being torn down). Not retried: any other refusal of HELLO (wrong token or version);
 *   waiting will not change the answer, so `run()` throws it.
 * - A handler that throws is reported to `onError` (default: the log) and the Worker keeps serving:
 *   one bad message must not take a chat server down. Pass an `onError` that rethrows to exit instead.
 * - stop(), SIGTERM and SIGINT (with ext-pcntl, see installSignalHandlers()) end run() after the
 *   handler in progress. Systemd and Kubernetes stop a worker with SIGTERM.
 */
final class Supervisor
{
    private const SLICE = 0.1;  // a backoff sleep is cut into slices so stop() is noticed quickly

    private bool $stopped = false;
    private ?Worker $worker = null;

    /** @var \Closure(): Client */
    private readonly \Closure $connect;
    /** @var \Closure(string): void */
    private readonly \Closure $log;
    /** @var \Closure(float): void */
    private readonly \Closure $sleep;
    /** @var \Closure(): float */
    private readonly \Closure $jitter;
    /** @var \Closure(): float */
    private readonly \Closure $now;
    /** @var \Closure(\Throwable): void */
    private readonly \Closure $onError;

    /**
     * @param \Closure(): Client                $connect    one connection attempt; throws on failure (forSocket() builds this)
     * @param float                             $minDelay   first pause after a failure, seconds
     * @param float                             $maxDelay   the pause never grows beyond this
     * @param float                             $resetAfter a session this long makes the next pause $minDelay again
     * @param (\Closure(string): void)|null     $log        one line per event; default writes to STDERR
     * @param (\Closure(float): void)|null      $sleep      for tests: pause for this many seconds
     * @param (\Closure(): float)|null          $jitter     for tests: a factor in [0.5, 1.0] applied to each pause
     * @param (\Closure(): float)|null          $now        for tests: the clock in seconds
     * @param (\Closure(\Throwable): void)|null $onError    a handler threw; default logs it
     */
    public function __construct(
        \Closure $connect,
        private readonly float $minDelay = 0.2,
        private readonly float $maxDelay = 5.0,
        private readonly float $resetAfter = 10.0,
        ?\Closure $log = null,
        ?\Closure $sleep = null,
        ?\Closure $jitter = null,
        ?\Closure $now = null,
        ?\Closure $onError = null,
    ) {
        if ($minDelay <= 0.0 || $maxDelay < $minDelay || $resetAfter < 0.0) {
            throw new \InvalidArgumentException('delays must satisfy 0 < minDelay <= maxDelay and resetAfter >= 0');
        }
        $this->connect = $connect;
        $this->log = $log ?? static function (string $line): void {
            fwrite(STDERR, '[wsrv] ' . $line . "\n");
        };
        $this->sleep = $sleep ?? [$this, 'pause'](...);
        $this->jitter = $jitter ?? static fn (): float => 0.5 + random_int(0, 500) / 1000;
        $this->now = $now ?? static fn (): float => microtime(true);
        $this->onError = $onError ?? function (\Throwable $e): void {
            ($this->log)('a handler threw ' . get_class($e) . ': ' . $e->getMessage());
        };
    }

    /** The usual case: connect to the daemon (a Unix socket path, or `tcp://host:port`) with the token each time. */
    public static function forSocket(string $socketPath, string $token = '', float $helloTimeout = 5.0): self
    {
        return new self(static fn (): Client => Client::connect($socketPath, $token, $helloTimeout));
    }

    /**
     * Serve until stop(). $configure receives each new Worker and registers its handlers.
     *
     * @param callable(Worker): void $configure
     * @throws HandshakeRefused when the daemon refuses HELLO for a reason waiting cannot fix
     */
    public function run(callable $configure): void
    {
        $delay = $this->minDelay;
        while (!$this->stopped) {
            $client = $this->tryConnect();
            if ($client === null) {
                $this->wait($delay);
                $delay = min($delay * 2, $this->maxDelay);
                continue;
            }
            $started = ($this->now)();
            $this->serve($client, $configure);
            if (($this->now)() - $started >= $this->resetAfter) {
                $delay = $this->minDelay;
            }
            if (!$this->stopped) {
                $this->wait($delay);
                $delay = min($delay * 2, $this->maxDelay);
            }
        }
    }

    /** Ask run() to return after the handler that is running now. Safe from a handler or a signal. */
    public function stop(): void
    {
        $this->stopped = true;
        $this->worker?->stop();
    }

    /**
     * Turn SIGTERM and SIGINT into stop(). Needs ext-pcntl (the CLI has it on most distributions).
     *
     * @return bool false when the extension is missing; the process then dies on those signals as usual
     */
    public function installSignalHandlers(): bool
    {
        if (!function_exists('pcntl_signal') || !function_exists('pcntl_async_signals')) {
            return false;
        }
        pcntl_async_signals(true);
        $handler = function (): void {
            $this->stop();
        };
        return pcntl_signal(SIGTERM, $handler) && pcntl_signal(SIGINT, $handler);
    }

    private function tryConnect(): ?Client
    {
        try {
            $client = ($this->connect)();
            ($this->log)('connected to wsrvd');
            return $client;
        } catch (HandshakeRefused $e) {
            if ($e->status !== StatusCode::BACKPRESSURE) {
                throw $e;
            }
            ($this->log)('wsrvd still has the previous application connected, retrying');
        } catch (WsrvException $e) {
            ($this->log)('cannot reach wsrvd: ' . $e->getMessage());
        }
        return null;
    }

    /** @param callable(Worker): void $configure */
    private function serve(Client $client, callable $configure): void
    {
        $worker = new Worker($client);
        $this->worker = $worker;
        try {
            $configure($worker);
            while (!$this->stopped) {
                try {
                    $worker->run();
                    return;  // stop() was called
                } catch (ConnectionClosed | ProtocolError $e) {
                    ($this->log)('lost wsrvd: ' . $e->getMessage());
                    return;
                } catch (\Throwable $e) {
                    ($this->onError)($e);  // the link is fine: keep serving
                }
            }
        } finally {
            $this->worker = null;
            $client->close();
        }
    }

    private function wait(float $backoff): void
    {
        if ($this->stopped) {
            return;
        }
        ($this->sleep)($backoff * ($this->jitter)());
    }

    /** The default sleep: in slices, so stop() (a signal) is noticed within a tenth of a second. */
    private function pause(float $seconds): void
    {
        $until = microtime(true) + $seconds;
        while (!$this->stopped && ($left = $until - microtime(true)) > 0.0) {
            usleep((int) (min($left, self::SLICE) * 1e6));
        }
    }
}
