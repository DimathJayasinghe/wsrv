<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * The app key and secret (the same two values Laravel's broadcasting configuration holds) and the
 * bounds the gateway keeps. Every bound has a default that is safe for a public site; each is
 * validated, and none can be zero or unlimited.
 *
 * The secret is only ever used inside this class (authorize() and verify()), is hidden from
 * print_r and var_dump, marked sensitive in stack traces, and cannot be serialized.
 */
final class GatewayConfig
{
    private const MIN_SECRET_BYTES = 16;
    /** Frames besides the subscribes that fit in the burst: pings, a whisper or two, a leave. */
    private const BURST_HEADROOM_MIN = 16;
    private const BURST_HEADROOM_DEFAULT = 64;
    private const MAX_BURST = 1_000_000;

    /** How many frames one connection may send at once; see the constructor. */
    public readonly int $messageBurst;

    /**
     * @param int $activityTimeout     seconds of silence after which pusher-js pings; sent to every browser
     * @param int $maxFrameBytes       a frame from a browser larger than this is refused (the daemon's
     *                                 max_message_size should be about the same, section "Deploying" of docs/LARAVEL.md)
     * @param int $maxChannelsPerConnection  also set the daemon's broker_max_topics_per_connection to at least this
     * @param int $maxPresenceMembers  members (users, not connections) of one presence channel
     * @param int $maxChannelDataBytes the signed channel_data of a presence member
     * @param bool $clientEvents       allow `client-*` events (whispers) between members of a private or presence channel
     * @param int $clientEventsPerSecond  whispers per connection per second
     * @param int $messagesPerSecond   all frames per connection, sustained: the rate at which the limiter's bucket refills
     * @param int|null $messageBurst   the bucket's size, the frames one connection may send at once. A page subscribes to
     *                                 every channel it uses in one burst, on load and again after each reconnect, before
     *                                 the daemon has answered any, so this cannot be below maxChannelsPerConnection + 16;
     *                                 null means maxChannelsPerConnection + 64
     * @param int $maxStrikes          protocol violations (a frame over the rate counts as one) before the connection is closed
     */
    public function __construct(
        public readonly string $appKey,
        #[\SensitiveParameter] private readonly string $appSecret,
        public readonly int $activityTimeout = 30,
        public readonly int $maxFrameBytes = 16384,
        public readonly int $maxChannelsPerConnection = 64,
        public readonly int $maxPresenceMembers = 100,
        public readonly int $maxChannelDataBytes = 2048,
        public readonly bool $clientEvents = true,
        public readonly int $clientEventsPerSecond = 10,
        public readonly int $messagesPerSecond = 30,
        ?int $messageBurst = null,
        public readonly int $maxStrikes = 8,
    ) {
        if (preg_match('/\A[A-Za-z0-9._-]{1,64}\z/', $appKey) !== 1) {
            throw new \InvalidArgumentException('appKey must be 1 to 64 letters, digits, dots, dashes or underscores');
        }
        if (strlen($appSecret) < self::MIN_SECRET_BYTES) {
            throw new \InvalidArgumentException('appSecret must be at least ' . self::MIN_SECRET_BYTES . ' bytes');
        }
        foreach ([
            'activityTimeout' => [$activityTimeout, 10, 3600],
            'maxFrameBytes' => [$maxFrameBytes, 64, 1 << 20],
            'maxChannelsPerConnection' => [$maxChannelsPerConnection, 1, 1024],
            'maxPresenceMembers' => [$maxPresenceMembers, 1, 100_000],
            'maxChannelDataBytes' => [$maxChannelDataBytes, 16, 65536],
            'clientEventsPerSecond' => [$clientEventsPerSecond, 1, 100_000],
            'messagesPerSecond' => [$messagesPerSecond, 1, 1_000_000],
            'maxStrikes' => [$maxStrikes, 1, 1_000_000],
        ] as $name => [$value, $min, $max]) {
            if ($value < $min || $value > $max) {
                throw new \InvalidArgumentException("$name must be between $min and $max");
            }
        }
        // Joins in progress need no cap of their own: each counts against the channel quota, so a connection
        // never has more than maxChannelsPerConnection of them. The burst has a floor for the same reason: a
        // page that joins all its channels at once must not be told off for it.
        $burst = $messageBurst ?? $maxChannelsPerConnection + self::BURST_HEADROOM_DEFAULT;
        $least = $maxChannelsPerConnection + self::BURST_HEADROOM_MIN;
        if ($burst < $least || $burst > self::MAX_BURST) {
            throw new \InvalidArgumentException(
                'messageBurst must be between ' . $least . ' (maxChannelsPerConnection + ' . self::BURST_HEADROOM_MIN . ') and ' . self::MAX_BURST,
            );
        }
        $this->messageBurst = $burst;
    }

    /** `<key>:<signature>` for a private or presence subscription; what Laravel's auth endpoint returns. */
    public function authorize(string $socketId, string $channel, ?string $channelData = null): string
    {
        return Signature::authorize($this->appKey, $this->appSecret, $socketId, $channel, $channelData);
    }

    public function verify(string $socketId, string $channel, ?string $channelData, string $auth): bool
    {
        return Signature::verify($this->appKey, $this->appSecret, $socketId, $channel, $channelData, $auth);
    }

    /** @return array<string, mixed> */
    public function __debugInfo(): array
    {
        return ['appKey' => $this->appKey, 'appSecret' => '(hidden)'];
    }

    public function __serialize(): array
    {
        throw new \LogicException('a GatewayConfig holds the app secret and cannot be serialized');
    }
}
