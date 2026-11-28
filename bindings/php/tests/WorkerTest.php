<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Client;
use Wsrv\Connection;
use Wsrv\Ipc\Codec;
use Wsrv\StatusCode;
use Wsrv\Worker;

/** @return array{0: Worker, 1: resource} */
function ready_worker(): array
{
    [$client, $daemon] = ready_client();
    return [new Worker($client), $daemon];
}

test('worker: open, message, close are dispatched in order with their arguments', function () {
    [$worker, $daemon] = ready_worker();
    $log = [];
    $worker->on('open', function (Connection $c, string $peer) use (&$log) {
        $log[] = ['open', $c->id, $peer];
    })->on('message', function (Connection $c, string $data, bool $binary) use (&$log) {
        $log[] = ['message', $c->id, $data, $binary];
    })->on('close', function (Connection $c, int $code, string $reason, bool $clean) use (&$log) {
        $log[] = ['close', $c->id, $code, $reason, $clean];
    });
    fwrite($daemon, event_bytes(1, 4, '127.0.0.1:9')
        . event_bytes(2, 4, 'hello')
        . event_bytes(3, 4, "\x01\x02")
        . event_bytes(4, 4, 'bye', 1000, 1));
    while ($worker->step(0.5)) {
        if (count($log) === 4) {
            break;
        }
    }
    assertSame([
        ['open', 4, '127.0.0.1:9'],
        ['message', 4, 'hello', false],
        ['message', 4, "\x01\x02", true],
        ['close', 4, 1000, 'bye', true],
    ], $log);
    assertSame(0, $worker->connectionCount(), 'the close event ends the connection');
});

test('worker: a handler can answer through its Connection', function () {
    [$worker, $daemon] = ready_worker();
    $worker->on('message', fn (Connection $c, string $data) => $c->send(strtoupper($data)));
    fwrite($daemon, event_bytes(1, 2, 'a') . event_bytes(2, 2, 'echo'));
    $worker->step(0.5);
    $worker->step(0.5);
    assertSame(Codec::sendText(1, 2, 'ECHO'), received($daemon));
});

test('worker: status answers reach the status handler', function () {
    [$worker, $daemon] = ready_worker();
    $seen = [];
    $worker->on('status', function (int $request, int $status) use (&$seen) {
        $seen[] = [$request, $status];
    });
    fwrite($daemon, status_frame(3, StatusCode::NO_SUCH_CONN));
    $worker->step(0.5);
    assertSame([[3, StatusCode::NO_SUCH_CONN]], $seen);
});

test('worker: a missing handler is not an error', function () {
    [$worker, $daemon] = ready_worker();
    fwrite($daemon, event_bytes(1, 1, 'a') . event_bytes(2, 1, 'b') . status_frame(1, 0) . pack('NCC', 2, 7, 0));
    for ($i = 0; $i < 4; $i++) {
        assertTrue($worker->step(0.5));
    }
    assertSame(1, $worker->connectionCount());
});

test('worker: run() returns after stop() is called from a handler', function () {
    [$worker, $daemon] = ready_worker();
    $worker->on('message', function () use ($worker) {
        $worker->stop();
    });
    fwrite($daemon, event_bytes(1, 1, 'a') . event_bytes(2, 1, 'stop now') . event_bytes(2, 1, 'not reached'));
    $worker->run(0.5);
    assertTrue($worker->step(0.5), 'the third event is still waiting');
});

test('worker: an exception from a handler leaves run() and the worker stays usable', function () {
    [$worker, $daemon] = ready_worker();
    $calls = 0;
    $worker->on('message', function () use (&$calls) {
        $calls++;
        if ($calls === 1) {
            throw new RuntimeException('handler bug');
        }
    });
    fwrite($daemon, event_bytes(1, 1, 'a') . event_bytes(2, 1, 'x') . event_bytes(2, 1, 'y'));
    $thrown = assertThrows(RuntimeException::class, fn () => $worker->run(0.5));
    assertSame('handler bug', $thrown->getMessage());
    $worker->step(0.5);
    assertSame(2, $calls, 'the next event is still delivered');
});

test('worker: a connection is forgotten even if its close handler throws', function () {
    [$worker, $daemon] = ready_worker();
    $worker->on('close', function () {
        throw new RuntimeException('close handler bug');
    });
    fwrite($daemon, event_bytes(1, 1, 'a') . event_bytes(4, 1, '', 1006, 0));
    $worker->step(0.5);
    assertThrows(RuntimeException::class, fn () => $worker->step(0.5));
    assertSame(0, $worker->connectionCount());
});

test('worker: an unknown handler name is refused', function () {
    [$worker] = ready_worker();
    assertThrows(InvalidArgumentException::class, fn () => $worker->on('mesage', fn () => null));
});

/** A clock the tests move by hand. */
function worker_clock(): object
{
    return new class {
        public float $now = 100.0;
    };
}

test('worker: every() calls a function about every N seconds while run() serves, and not before', function () {
    [$client, $daemon] = ready_client();
    $clock = worker_clock();
    $worker = new Worker($client, fn (): float => $clock->now);
    $fired = [];
    $worker->every(10.0, function () use (&$fired, $clock) {
        $fired[] = $clock->now;
    });
    $messages = 0;
    $worker->on('message', function () use (&$messages, $clock, $worker) {
        $messages++;
        $clock->now += 4.0;                       // each message "takes" four seconds
        if ($messages === 7) {
            $worker->stop();
        }
    });
    fwrite($daemon, event_bytes(1, 1, 'a') . str_repeat(event_bytes(2, 1, 'm'), 7));
    $worker->run(0.5);
    // the clock reads 104, 108, 112 (due since 110: fires, next 122), 116, 120, 124 (fires, next 134), 128 and stop()
    assertSame([112.0, 124.0], $fired);
});

test('worker: every() does not catch up on the intervals it missed, and a run that is stopped does not call it', function () {
    [$client, $daemon] = ready_client();
    $clock = worker_clock();
    $worker = new Worker($client, fn (): float => $clock->now);
    $fired = 0;
    $worker->every(10.0, function () use (&$fired) {
        $fired++;
    });
    $messages = 0;
    $worker->on('message', function () use (&$messages, $clock, $worker) {
        $clock->now += 1000.0;                    // a handler that took 100 intervals
        if (++$messages === 3) {
            $worker->stop();
        }
    });
    fwrite($daemon, event_bytes(1, 1, 'a') . str_repeat(event_bytes(2, 1, 'm'), 3));
    $worker->run(0.5);
    assertSame(2, $fired, 'one call after each of the first two messages, not one per missed interval, and none after stop()');
});

test('worker: an exception from a timer leaves run() like one from a handler, and the timer is already scheduled again', function () {
    [$client, $daemon] = ready_client();
    $clock = worker_clock();
    $worker = new Worker($client, fn (): float => $clock->now);
    $calls = 0;
    $worker->every(10.0, function () use (&$calls) {
        $calls++;
        throw new RuntimeException('timer bug');
    });
    $worker->on('message', function () use ($clock) {
        $clock->now += 11.0;
    });
    fwrite($daemon, event_bytes(1, 1, 'a') . event_bytes(2, 1, 'm') . event_bytes(2, 1, 'm') . event_bytes(2, 1, 'm'));
    $thrown = assertThrows(RuntimeException::class, fn () => $worker->run(0.5));
    assertSame('timer bug', $thrown->getMessage());
    assertSame(1, $calls);
    $seen = 0;
    $worker->on('message', function () use (&$seen, $clock, $worker) {
        $clock->now += 1.0;                       // 112, 113: before the next call at 121
        if (++$seen === 2) {
            $worker->stop();
        }
    });
    $worker->run(0.5);
    assertSame(1, $calls, 'a timer that threw is not called again at once');
});

test('worker: every() refuses an interval that is not a positive finite number', function () {
    [$worker] = ready_worker();
    foreach ([0.0, -1.0, NAN, INF] as $bad) {
        assertThrows(InvalidArgumentException::class, fn () => $worker->every($bad, fn () => null));
    }
});
