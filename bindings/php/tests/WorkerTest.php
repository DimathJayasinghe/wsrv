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
