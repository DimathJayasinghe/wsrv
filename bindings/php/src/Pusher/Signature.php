<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * The proof that the application let a browser join a private or presence channel.
 *
 * The browser asks the application's own endpoint (Laravel's /broadcasting/auth), which decides and
 * signs; the browser hands the signature to the gateway, which checks it with the same secret. The
 * value is `<app key>:<hex HMAC-SHA256>` over `<socket id>:<channel>` and, for a presence channel,
 * `:<channel_data>` (the exact string the browser sends back). Binding the socket id means a
 * signature cannot be used by another connection, and binding the channel_data means a user cannot
 * change who they are after being signed.
 */
final class Signature
{
    public static function authorize(
        string $key,
        string $secret,
        string $socketId,
        string $channel,
        ?string $channelData = null,
    ): string {
        $signed = $socketId . ':' . $channel;
        if ($channelData !== null && $channelData !== '') {
            $signed .= ':' . $channelData;
        }
        return $key . ':' . hash_hmac('sha256', $signed, $secret);
    }

    /** Compared in constant time (RFC-independent, but a timing difference would reveal the signature). */
    public static function verify(
        string $key,
        string $secret,
        string $socketId,
        string $channel,
        ?string $channelData,
        string $auth,
    ): bool {
        return hash_equals(self::authorize($key, $secret, $socketId, $channel, $channelData), $auth);
    }
}
