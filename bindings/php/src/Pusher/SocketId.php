<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * `<connection id>.<random number>`: the shape pusher-js and Laravel expect (two numbers and a dot).
 * The gateway builds it, so a browser never chooses it. Laravel hands it back as `X-Socket-ID` when a
 * request should not be echoed to its own sender (`toOthers()`), and the connection id in front is
 * what lets the broadcaster say "except this connection". That is all it is used for: a forged id
 * can at worst leave one other connection out of one broadcast, and every signature is bound to the
 * real socket id of the connection that subscribes.
 */
final class SocketId
{
    public static function generate(int $connection): string
    {
        return $connection . '.' . random_int(1, 1_000_000_000);
    }

    /** @return int|null the connection id, or null if $socketId is not a well-formed one */
    public static function connection(string $socketId): ?int
    {
        if (preg_match('/\A([1-9][0-9]{0,18})\.[0-9]{1,10}\z/', $socketId, $m) !== 1) {
            return null;
        }
        $id = (int) $m[1];
        return (string) $id === $m[1] ? $id : null;   // more than PHP_INT_MAX saturates, and then differs
    }
}
