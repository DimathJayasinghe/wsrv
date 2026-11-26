<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * Channel names. The kind comes from the prefix, as in the Pusher protocol and in Laravel
 * (`private-`, `presence-`, `private-encrypted-`). A name is also a wsrv room name, so it must be
 * one the daemon accepts: 1 to 128 bytes of [A-Za-z0-9._:/@-] (ADR-0024). Pusher also allows `=`, `,`
 * and `;` and 164 bytes; those names are refused, never rewritten, because a rewrite could make two
 * different names share one room (ADR-0042 section 3, rule 4).
 */
final class ChannelName
{
    public const MAX_BYTES = 128;

    /** Cache channels keep the last event for late joiners; that feature is not implemented. */
    private const CACHE_PREFIXES = ['cache-', 'private-cache-', 'presence-cache-'];

    public static function kind(string $name): ChannelKind
    {
        if (str_starts_with($name, 'presence-')) {
            return ChannelKind::Presence;
        }
        if (str_starts_with($name, 'private-')) {
            return ChannelKind::Private;
        }
        return ChannelKind::Public;
    }

    /** @return string|null why the name cannot be used, or null if it can */
    public static function problem(string $name): ?string
    {
        if ($name === '' || strlen($name) > self::MAX_BYTES) {
            return 'Channel name must be 1 to ' . self::MAX_BYTES . ' bytes';
        }
        if (preg_match('/\A[A-Za-z0-9._:\/@-]+\z/', $name) !== 1) {
            return 'Channel name has a character that is not allowed';
        }
        foreach (self::CACHE_PREFIXES as $prefix) {
            if (str_starts_with($name, $prefix)) {
                return 'Cache channels are not supported';
            }
        }
        return null;
    }
}
