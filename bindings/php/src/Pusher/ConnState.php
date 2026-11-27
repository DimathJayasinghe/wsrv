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
    /** Recent violations: one is forgiven for each quiet Gateway::STRIKE_FORGIVE_SECONDS before the next (see Gateway::strike()). */
    public int $strikes = 0;
    /** When the previous strike happened (or the connection opened). */
    public float $strikeStamp;
    /** The gateway asked the daemon to close it; further frames are ignored. */
    public bool $closing = false;
    /** The message limiter's bucket (Gateway::takeMessageToken): frames left in it, and when it was last refilled. */
    public float $messageTokens;
    public float $messageStamp;
    public int $whisperWindow = 0;
    public int $whisperCount = 0;

    public function __construct(public readonly string $socketId, float $messageTokens, float $now)
    {
        $this->messageTokens = $messageTokens;
        $this->messageStamp = $now;
        $this->strikeStamp = $now;
    }
}
