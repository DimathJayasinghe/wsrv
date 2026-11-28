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
    /** @var array<int, true> sends of a join's answer to the daemon, whose STATUS has not come back yet */
    public array $sends = [];
    /** Recent violations: one is forgiven for each quiet Gateway::STRIKE_FORGIVE_SECONDS before the next (see Gateway::strike()). */
    public int $strikes = 0;
    /** When the previous strike happened (or the connection opened). */
    public float $strikeStamp;
    /**
     * A signature for a private or presence channel verified for this connection, so it is not an anonymous visitor: it may
     * take the part of the public-name budget that is kept back for such connections (ADR-0042 A4). It lasts as long as the
     * connection, whatever channels it leaves.
     */
    public bool $verified = false;
    /**
     * @var array<string, float> public channels this connection asked to join while only the part of the public-name budget kept
     *                           for connections that proved who they are was left => when it asked (ADR-0042 A4). They hold no
     *                           name and ask nothing of the daemon; each counts against the channel quota. They are admitted or
     *                           refused when a signature verifies, or after Gateway::PARK_SECONDS.
     */
    public array $parked = [];
    /** The gateway asked the daemon to close it; further frames are ignored. */
    public bool $closing = false;
    /** @var array{int, string}|null the close code and reason to ask for again if the daemon refuses the close */
    public ?array $closeAs = null;
    /** The request id of the latest close asked for (0 = none); closed() forgets it if the answer never came. */
    public int $closeRequest = 0;
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
