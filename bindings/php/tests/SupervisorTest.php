<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Client;
use Wsrv\Connection;
use Wsrv\ConnectionClosed;
use Wsrv\HandshakeRefused;
use Wsrv\MessageTooLarge;
use Wsrv\StatusCode;
use Wsrv\Supervisor;
use Wsrv\Worker;
use Wsrv\WsrvException;

// The reconnecting runner (docs/DEPLOYMENT.md section 4): the daemon is restarted, the worker must
// follow. Everything time-related is injected, so these tests neither sleep nor wait.

/** A Supervisor over scripted connection attempts. Each step is a Throwable to throw or a Client to return. */
function scripted(array $steps, array &$sleeps, ?Closure $now = null, ?Closure $onError = null): Supervisor
{
    $lines = [];
    return new Supervisor(
        connect: function () use (&$steps): Client {
            $step = array_shift($steps);
            if ($step === null) {
                throw new LogicException('the test script ran out of connection attempts');
            }
            if ($step instanceof Throwable) {
                throw $step;
            }
            return $step;
        },
        minDelay: 0.1,
        maxDelay: 0.5,
        log: function (string $line) use (&$lines): void {
            $lines[] = $line;
        },
        sleep: function (float $seconds) use (&$sleeps): void {
            $sleeps[] = $seconds;
        },
        jitter: fn (): float => 1.0,
        now: $now,
        onError: $onError,
    );
}

test('supervisor: connection attempts back off by doubling up to the cap', function () {
    $sleeps = [];
    $count = 0;
    $supervisor = null;
    $supervisor = new Supervisor(
        connect: function () use (&$count): Client {
            $count++;
            throw new WsrvException('cannot connect');
        },
        minDelay: 0.1,
        maxDelay: 0.5,
        log: fn (string $line) => null,
        sleep: function (float $seconds) use (&$sleeps, &$supervisor, &$count): void {
            $sleeps[] = $seconds;
            if ($count === 6) {
                $supervisor->stop();
            }
        },
        jitter: fn (): float => 1.0,
    );
    $supervisor->run(function (Worker $w): void {
    });
    assertSame([0.1, 0.2, 0.4, 0.5, 0.5, 0.5], $sleeps);
});

test('supervisor: jitter keeps each delay between half and all of the backoff', function () {
    $sleeps = [];
    $count = 0;
    $supervisor = null;
    $supervisor = new Supervisor(
        connect: function () use (&$count): Client {
            $count++;
            throw new WsrvException('cannot connect');
        },
        minDelay: 1.0,
        maxDelay: 8.0,
        log: fn (string $line) => null,
        sleep: function (float $seconds) use (&$sleeps, &$supervisor, &$count): void {
            $sleeps[] = $seconds;
            if ($count === 40) {
                $supervisor->stop();
            }
        },
    );
    $supervisor->run(function (Worker $w): void {
    });
    $backoff = 1.0;
    foreach ($sleeps as $slept) {
        assertTrue($slept >= $backoff / 2 - 1e-9 && $slept <= $backoff + 1e-9, "delay $slept within [$backoff/2, $backoff]");
        $backoff = min($backoff * 2, 8.0);
    }
});

test('supervisor: a daemon that goes away is followed by a fresh Worker on the next connection', function () {
    [$first, $daemon1] = ready_client();
    [$second, $daemon2] = ready_client();
    fwrite($daemon1, event_bytes(1, 7, '10.0.0.1:1') . event_bytes(2, 7, 'one'));
    fclose($daemon1);  // the daemon restarts
    fwrite($daemon2, event_bytes(1, 9, '10.0.0.2:2') . event_bytes(2, 9, 'two'));
    $sleeps = [];
    $seen = [];
    $configured = 0;
    $supervisor = scripted([$first, $second], $sleeps);
    $supervisor->run(function (Worker $worker) use (&$seen, &$configured, &$supervisor): void {
        $configured++;
        $worker->on('message', function (Connection $c, string $data) use (&$seen, &$supervisor): void {
            $seen[] = [$c->id, $data];
            if ($data === 'two') {
                $supervisor->stop();
            }
        });
    });
    assertSame(2, $configured, 'configure runs for every new Worker');
    assertSame([[7, 'one'], [9, 'two']], $seen);
    assertSame([0.1], $sleeps, 'one pause between the daemon going away and the next attempt');
});

test('supervisor: another application being connected is retried, a wrong token is not', function () {
    [$client, $daemon] = ready_client();
    fwrite($daemon, event_bytes(2, 3, 'go'));
    $sleeps = [];
    $supervisor = scripted([
        new HandshakeRefused(StatusCode::BACKPRESSURE),  // the old worker's link is not gone yet
        new HandshakeRefused(StatusCode::BACKPRESSURE),
        $client,
    ], $sleeps);
    $supervisor->run(function (Worker $worker) use (&$supervisor): void {
        $worker->on('message', function () use (&$supervisor): void {
            $supervisor->stop();
        });
    });
    assertSame([0.1, 0.2], $sleeps);

    $none = [];
    $wrong = scripted([new HandshakeRefused(StatusCode::INVALID_ARGUMENT)], $none);
    $e = assertThrows(HandshakeRefused::class, fn () => $wrong->run(function (Worker $w): void {
    }));
    assertSame(StatusCode::INVALID_ARGUMENT, $e->status);
    assertSame([], $none, 'no point in waiting: the answer will not change');
});

test('supervisor: a handler that throws is reported and the worker keeps serving', function () {
    [$client, $daemon] = ready_client();
    fwrite($daemon, event_bytes(2, 3, 'boom') . event_bytes(2, 3, 'fine'));
    $sleeps = [];
    $errors = [];
    $seen = [];
    $supervisor = scripted([$client], $sleeps, null, function (Throwable $e) use (&$errors): void {
        $errors[] = $e->getMessage();
    });
    $supervisor->run(function (Worker $worker) use (&$seen, &$supervisor): void {
        $worker->on('message', function (Connection $c, string $data) use (&$seen, &$supervisor): void {
            if ($data === 'boom') {
                throw new RuntimeException('bug in a handler');
            }
            $seen[] = $data;
            $supervisor->stop();
        });
    });
    assertSame(['bug in a handler'], $errors);
    assertSame(['fine'], $seen);
    assertSame([], $sleeps, 'the link was never lost: no reconnect');
});

test('supervisor: a message too large for the daemon is reported like a handler error, and the link is kept', function () {
    [$client, $daemon] = ready_client(4096);
    fwrite($daemon, event_bytes(2, 3, 'big') . event_bytes(2, 3, 'small'));
    $sleeps = [];
    $errors = [];
    $seen = [];
    $supervisor = scripted([$client], $sleeps, null, function (Throwable $e) use (&$errors): void {
        $errors[] = get_class($e);
    });
    $supervisor->run(function (Worker $worker) use (&$seen, &$supervisor): void {
        $worker->on('message', function (Connection $c, string $data) use (&$seen, &$supervisor): void {
            if ($data === 'big') {
                $c->send(str_repeat('x', 5000));     // more than the daemon's 4096-byte frame
            }
            $seen[] = $data;
            if ($data === 'small') {
                $supervisor->stop();
            }
        });
    });
    assertSame([MessageTooLarge::class], $errors);
    assertSame(['small'], $seen, 'the next event was handled on the same link');
    assertSame([], $sleeps, 'the link was never lost: no reconnect, no backoff');
});

test('supervisor: a protocol error from the daemon still drops the link and reconnects', function () {
    [$client, $daemon] = ready_client();
    fwrite($daemon, pack('N', 2) . "\x7f\x00");    // a frame type the daemon never sends
    [$second, $daemon2] = ready_client();
    $sleeps = [];
    $supervisor = null;
    $supervisor = scripted([$client, $second], $sleeps);
    $connected = 0;
    $supervisor->run(function (Worker $worker) use (&$connected, &$supervisor): void {
        if (++$connected === 2) {
            $supervisor->stop();
        }
    });
    assertSame(2, $connected);
    assertSame(1, count($sleeps), 'one backoff between the two links');
});

test('supervisor: the backoff starts over after a session that lasted, but not after a flapping one', function () {
    $time = 100.0;
    $now = function () use (&$time): float {
        return $time;
    };
    [$a, $da] = ready_client();
    [$b, $db] = ready_client();
    [$c, $dc] = ready_client();
    fclose($da);  // dies at once: a flapping daemon (0 s session)
    fwrite($db, event_bytes(2, 3, 'long'));
    fclose($db);  // dies after a long session (the handler moves the clock)
    fwrite($dc, event_bytes(2, 3, 'stop'));
    $sleeps = [];
    $supervisor = scripted([
        new WsrvException('down'), new WsrvException('down'),  // 0.1, 0.2
        $a,                                                    // flapping: next delay 0.4
        $b,                                                    // long session: delay resets to 0.1
        $c,
    ], $sleeps, $now);
    $supervisor->run(function (Worker $worker) use (&$time, &$supervisor): void {
        $worker->on('message', function (Connection $conn, string $data) use (&$time, &$supervisor): void {
            if ($data === 'long') {
                $time += 60.0;
            } else {
                $supervisor->stop();
            }
        });
    });
    assertSame([0.1, 0.2, 0.4, 0.1], $sleeps);
});

test('supervisor: stop() before run() or from a handler ends run() without another attempt', function () {
    $sleeps = [];
    $attempts = 0;
    $supervisor = new Supervisor(
        connect: function () use (&$attempts): Client {
            $attempts++;
            throw new WsrvException('down');
        },
        minDelay: 0.1,
        maxDelay: 0.5,
        log: fn (string $line) => null,
        sleep: function (float $seconds) use (&$sleeps): void {
            $sleeps[] = $seconds;
        },
    );
    $supervisor->stop();
    $supervisor->run(function (Worker $w): void {
    });
    assertSame(0, $attempts, 'stopped before the first attempt: nothing is tried');
});

test('supervisor: bad settings are refused up front', function () {
    foreach ([[0.0, 1.0], [-1.0, 1.0], [1.0, 0.5], [0.1, 0.0]] as [$min, $max]) {
        assertThrows(InvalidArgumentException::class, fn () => new Supervisor(
            connect: fn () => throw new LogicException('unused'),
            minDelay: $min,
            maxDelay: $max,
        ), "min $min max $max");
    }
});
