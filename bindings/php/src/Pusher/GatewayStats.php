<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * What a running Gateway holds and what it has done (ADR-0042 A2), as numbers only.
 *
 * Gauges say what is held now: when every browser is gone each of them is zero again, which is how a
 * leak shows. Counters only grow, from the moment this Gateway was made (`wsrv:echo` makes a new one
 * each time it connects to the daemon): the difference between two readings is what happened in
 * between (frames and joins per interval, a burst of 4009 or 4200 answers). A counter that went down
 * means the gateway was made again, that is, the worker lost the daemon.
 * Nothing here is a string a browser sent, so the line cannot be used to inject text into a log.
 */
final class GatewayStats
{
    public function __construct(
        /** Browsers connected through this gateway. */
        public readonly int $connections,
        /** Channels joined, summed over the connections. */
        public readonly int $channels,
        /** Joins the daemon has not answered yet. */
        public readonly int $joinsInProgress,
        public readonly int $presenceChannels,
        /** Users in presence channels (a user with two tabs counts once). */
        public readonly int $presenceMembers,
        /** `memory_get_usage()` of the PHP process: the application's own memory is in it too. */
        public readonly int $memoryBytes,
        /** Connections accepted. */
        public readonly int $opened,
        /** Frames received from browsers, answered or refused. */
        public readonly int $frames,
        /** Channels joined (a repeated subscribe is not a join). */
        public readonly int $joins,
        /** Answers 4200: a frame that was not valid Pusher (each is a strike). */
        public readonly int $badFrames,
        /** Answers 4009: a signature that did not verify, or a whisper from a non-member. */
        public readonly int $unauthorized,
        /** Answers 4301 to a frame over the message bucket, or a whisper over its rate. */
        public readonly int $rateLimited,
        /** `pusher:subscription_error` frames (4300): channel name, quota, presence cap, daemon refusal. */
        public readonly int $subscriptionErrors,
        /**
         * Commands the daemon refused: a join over a broker bound (`broker_max_topics` 1024,
         * `broker_max_subscriptions` 100 000, `broker_max_subscribers_per_topic` 10 000 by default) or with another
         * error status, and a join's answer it could not queue (that browser is closed with 1011). Rising means
         * the daemon is too small for the site, not that a browser misbehaved.
         */
        public readonly int $daemonRefused,
        /** Connections the gateway closed with 1008 for collecting `max_strikes`. */
        public readonly int $strikeCloses,
    ) {
    }

    /**
     * The same numbers with fixed names, in a fixed order (the order of the line).
     *
     * @return array<string, int>
     */
    public function toArray(): array
    {
        return [
            'connections' => $this->connections,
            'channels' => $this->channels,
            'joins_in_progress' => $this->joinsInProgress,
            'presence_channels' => $this->presenceChannels,
            'presence_members' => $this->presenceMembers,
            'memory_bytes' => $this->memoryBytes,
            'opened' => $this->opened,
            'frames' => $this->frames,
            'joins' => $this->joins,
            'bad_frames' => $this->badFrames,
            'unauthorized' => $this->unauthorized,
            'rate_limited' => $this->rateLimited,
            'subscription_errors' => $this->subscriptionErrors,
            'daemon_refused' => $this->daemonRefused,
            'strike_closes' => $this->strikeCloses,
        ];
    }

    /** One line of `key=value` pairs for a log: `connections=12 channels=40 ... strike_closes=0`. */
    public function line(): string
    {
        $pairs = [];
        foreach ($this->toArray() as $key => $value) {
            $pairs[] = $key . '=' . $value;
        }
        return implode(' ', $pairs);
    }
}
