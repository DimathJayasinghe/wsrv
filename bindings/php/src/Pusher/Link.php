<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * What the Gateway needs from the daemon, as five commands. WorkerLink is the real one (a Worker);
 * the tests use one that only records. Every call is a command the daemon answers later; the
 * request id of subscribe() and unsubscribe() comes back in a STATUS the caller passes to
 * Gateway::status().
 */
interface Link
{
    public function send(int $conn, string $text): void;

    /** @return int the request id */
    public function subscribe(int $conn, string $topic): int;

    /** @return int the request id */
    public function unsubscribe(int $conn, string $topic): int;

    /** To every subscriber of $topic except connection $except (0 = nobody). */
    public function publish(string $topic, string $payload, int $except): void;

    public function close(int $conn, int $code, string $reason): void;
}
