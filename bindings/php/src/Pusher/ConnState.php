<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/** What the Gateway knows about one connection. @internal */
final class ConnState
{
    /** @var array<string, string|null> channels this connection is in => the presence user id, or null */
    public array $channels = [];
    /** @var array<int, true> subscribe requests sent to the daemon and not yet answered */
    public array $pending = [];
    public int $strikes = 0;
    /** The gateway asked the daemon to close it; further frames are ignored. */
    public bool $closing = false;
    public int $messageWindow = 0;
    public int $messageCount = 0;
    public int $whisperWindow = 0;
    public int $whisperCount = 0;

    public function __construct(public readonly string $socketId)
    {
    }
}
