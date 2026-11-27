<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

use Wsrv\Connection;
use Wsrv\StatusCode;
use Wsrv\Upgrade;
use Wsrv\Worker;

/**
 * The Pusher WebSocket protocol, spoken by the PHP worker (ADR-0042), so that Laravel Echo and
 * pusher-js can talk to wsrvd.
 *
 * The daemon already holds the sockets, the rooms and the fan-out; this class only decides who may
 * join which room and keeps the member list of presence channels. A broadcast never passes through
 * here: Laravel publishes to the room (through Wsrv\Publisher) and the daemon delivers it.
 *
 *     $worker = new Worker(Client::connect('/run/wsrv/ipc.sock'));
 *     Gateway::forWorker(new GatewayConfig($appKey, $appSecret), $worker);
 *     $worker->run();
 *
 * Every frame from a browser is untrusted. The gateway bounds its size and rate, refuses a
 * malformed one with a `pusher:error` and a strike, closes a connection that collects too many
 * strikes, and never lets a bad frame raise an exception.
 *
 * It owns the worker's open, message, close and status handlers. An application that needs those
 * for something else calls opened(), received(), closed() and status() from its own handlers.
 */
final class Gateway
{
    private const ERR_BAD_FRAME = 4200;
    private const ERR_UNAUTHORIZED = 4009;
    private const ERR_CLIENT_EVENTS = 4301;
    private const SUBSCRIPTION_ERROR = 4300;
    private const CLOSE_POLICY = 1008;
    /** One strike is forgiven for each full period of this many quiet seconds (ADR-0042 A2). A constant: the knob for a site is max_strikes. */
    private const STRIKE_FORGIVE_SECONDS = 60.0;

    /** @var array<int, ConnState> */
    private array $conns = [];
    /**
     * Joins waiting for the daemon's answer, by request id. Bounded: each one counts against its
     * connection's channel quota, so there are never more than connections x maxChannelsPerConnection,
     * and a connection that sends faster than its message rate is refused before it can add one.
     *
     * @var array<int, array{conn: int, channel: string, userId: ?string, info: ?array, cancelled: bool}>
     */
    private array $pending = [];
    private readonly PresenceTable $presence;
    private readonly \Closure $clock;
    // Counters since the start of the process, for stats(). Only ever incremented.
    private int $opened = 0;
    private int $frames = 0;
    private int $joins = 0;
    private int $badFrames = 0;
    private int $unauthorized = 0;
    private int $rateLimited = 0;
    private int $subscriptionErrors = 0;
    private int $strikeCloses = 0;

    /** @param callable(): float|null $clock seconds as a float; for tests */
    public function __construct(
        private readonly GatewayConfig $config,
        private readonly Link $link,
        ?callable $clock = null,
    ) {
        $this->presence = new PresenceTable($config->maxPresenceMembers);
        // Monotonic, so a step of the wall clock gives no credit to the limiters and takes none away.
        $this->clock = $clock !== null ? \Closure::fromCallable($clock) : static fn (): float => hrtime(true) / 1e9;
    }

    /**
     * Wire a gateway into a Worker: it takes the open, message, close and status handlers. With
     * $checkAppKey the daemon must run with `handshake_hook = true` (and then needs this handler,
     * which refuses a URL that does not carry the app key with 404).
     */
    public static function forWorker(GatewayConfig $config, Worker $worker, bool $checkAppKey = false): self
    {
        $link = new WorkerLink($worker);
        $gateway = new self($config, $link);
        $worker->on('open', function (Connection $c) use ($link, $gateway): void {
            $link->remember($c);
            $gateway->opened($c->id);
        });
        $worker->on('message', function (Connection $c, string $data, bool $binary) use ($gateway): void {
            $binary ? $gateway->malformed($c->id) : $gateway->received($c->id, $data);
        });
        $worker->on('close', function (Connection $c) use ($link, $gateway): void {
            $gateway->closed($c->id);
            $link->forget($c->id);
        });
        $worker->on('status', static function (int $request, int $status) use ($gateway): void {
            $gateway->status($request, $status);
        });
        if ($checkAppKey) {
            $worker->on('authenticate', static fn (Upgrade $u): bool|int => $gateway->acceptsPath($u->request->path) ? true : 404);
        }
        return $gateway;
    }

    /** True if a WebSocket URL path is this app's: `/app/<key>`, optionally after a prefix. */
    public function acceptsPath(string $path): bool
    {
        return str_ends_with($path, '/app/' . $this->config->appKey);
    }

    // ---- events from the worker ---------------------------------------------------------------

    public function opened(int $conn): void
    {
        $state = new ConnState(SocketId::generate($conn), (float) $this->config->messageBurst, ($this->clock)());
        $this->conns[$conn] = $state;
        $this->opened++;
        $this->link->send($conn, Frame::encode('pusher:connection_established', [
            'socket_id' => $state->socketId,
            'activity_timeout' => $this->config->activityTimeout,
        ]));
    }

    public function received(int $conn, string $text): void
    {
        $state = $this->conns[$conn] ?? null;
        if ($state === null || $state->closing) {
            return;
        }
        $this->frames++;
        if (!$this->takeMessageToken($state)) {
            $this->rateLimited++;
            $this->error($conn, self::ERR_CLIENT_EVENTS, 'Rate limit exceeded');
            $this->strike($conn, $state);
            return;
        }
        try {
            $frame = Frame::decode($text, $this->config->maxFrameBytes);
        } catch (BadFrame) {
            $this->invalid($conn, $state);
            return;
        }
        if (str_starts_with($frame->event, 'pusher:')) {
            match ($frame->event) {
                'pusher:ping' => $this->link->send($conn, Frame::encode('pusher:pong')),
                'pusher:pong' => null,
                'pusher:subscribe' => $this->subscribe($conn, $state, $frame),
                'pusher:unsubscribe' => $this->unsubscribe($conn, $state, $frame),
                'pusher:signin' => $this->error($conn, self::ERR_BAD_FRAME, 'User authentication is not supported.'),
                default => $this->invalid($conn, $state),
            };
        } elseif (str_starts_with($frame->event, 'client-')) {
            $this->whisper($conn, $state, $frame);
        }
        // Anything else is not for the gateway (an application event name sent by a browser): ignored.
    }

    /** A binary message, or anything else the gateway cannot read, counts as a bad frame. */
    public function malformed(int $conn): void
    {
        $state = $this->conns[$conn] ?? null;
        if ($state !== null && !$state->closing) {
            $this->frames++;
            $this->invalid($conn, $state);
        }
    }

    public function closed(int $conn): void
    {
        $state = $this->conns[$conn] ?? null;
        if ($state === null) {
            return;
        }
        unset($this->conns[$conn]);
        foreach (array_keys($state->pending) as $request) {
            unset($this->pending[$request]);
        }
        // The daemon dropped this connection's rooms when it closed; only the member list is ours.
        foreach ($state->channels as $channel => $userId) {
            if ($userId !== null) {
                $this->memberLeft((string) $channel, $conn, $userId);
            }
        }
    }

    /** The daemon's answer to a subscribe this gateway sent; answers to other commands are ignored. */
    public function status(int $request, int $status): void
    {
        $join = $this->pending[$request] ?? null;
        if ($join === null) {
            return;
        }
        unset($this->pending[$request]);
        $state = $this->conns[$join['conn']] ?? null;
        if ($state === null) {
            return;
        }
        unset($state->pending[$request]);
        if ($join['cancelled']) {
            // The browser left before the daemon answered: if the daemon did put it in the room, take it out.
            if ($status === StatusCode::OK) {
                $this->link->unsubscribe($join['conn'], $join['channel']);
            }
            return;
        }
        if ($status === StatusCode::OK) {
            $this->joined($join['conn'], $state, $join['channel'], $join['userId'], $join['info']);
        } elseif ($status === StatusCode::LIMIT) {
            $this->subscriptionError($join['conn'], $join['channel'], 'LimitReached', 'The server is at its channel limit');
        } elseif ($status !== StatusCode::NO_SUCH_CONN) {
            $this->subscriptionError($join['conn'], $join['channel'], 'ServerError', 'Subscription could not be completed');
        }
    }

    // ---- for tests and metrics ----------------------------------------------------------------

    /**
     * What the gateway holds and has done (ADR-0042 A2). It walks the connections, so call it once in a
     * while (the `wsrv:echo` command does it every `--stats-interval` seconds), not per frame.
     */
    public function stats(): GatewayStats
    {
        $channels = 0;
        foreach ($this->conns as $state) {
            $channels += count($state->channels);
        }
        return new GatewayStats(
            connections: count($this->conns),
            channels: $channels,
            joinsInProgress: count($this->pending),
            presenceChannels: $this->presence->channels(),
            presenceMembers: $this->presence->members(),
            memoryBytes: memory_get_usage(),
            opened: $this->opened,
            frames: $this->frames,
            joins: $this->joins,
            badFrames: $this->badFrames,
            unauthorized: $this->unauthorized,
            rateLimited: $this->rateLimited,
            subscriptionErrors: $this->subscriptionErrors,
            strikeCloses: $this->strikeCloses,
        );
    }

    public function channelsOf(int $conn): int
    {
        return count($this->conns[$conn]->channels ?? []);
    }

    public function pendingCount(): int
    {
        return count($this->pending);
    }

    public function presenceChannels(): int
    {
        return $this->presence->channels();
    }

    // ---- pusher:subscribe ---------------------------------------------------------------------

    private function subscribe(int $conn, ConnState $state, Frame $frame): void
    {
        $data = $frame->object();
        $channel = $data['channel'] ?? null;
        $channelData = $data['channel_data'] ?? null;
        if ($data === null || !is_string($channel) || ($channelData !== null && !is_string($channelData))) {
            $this->invalid($conn, $state);
            return;
        }
        $problem = ChannelName::problem($channel);
        if ($problem !== null) {
            $this->subscriptionError($conn, $channel, 'InvalidChannelName', $problem);
            return;
        }
        if (array_key_exists($channel, $state->channels)) {
            $this->sendSucceeded($conn, $channel);   // already in: say so again, change nothing
            return;
        }
        $revived = null;   // a join that was cancelled by an unsubscribe and has not been answered yet
        foreach (array_keys($state->pending) as $request) {
            if ($this->pending[$request]['channel'] === $channel) {
                if (!$this->pending[$request]['cancelled']) {
                    return;                          // a join for it is already on its way
                }
                $revived = $request;
                break;
            }
        }
        if ($revived === null) {
            if (count($state->channels) + count($state->pending) >= $this->config->maxChannelsPerConnection) {
                $this->subscriptionError($conn, $channel, 'LimitReached', 'Connection is over channel quota');
                return;
            }
        }

        $kind = ChannelName::kind($channel);
        $userId = null;
        $info = null;
        if ($kind !== ChannelKind::Public) {
            $auth = $data['auth'] ?? null;
            $signed = $kind === ChannelKind::Presence ? $channelData : null;
            if (!is_string($auth) || !$this->config->verify($state->socketId, $channel, $signed, $auth)) {
                $this->error($conn, self::ERR_UNAUTHORIZED, 'Connection is unauthorized');
                return;
            }
            if ($kind === ChannelKind::Presence) {
                $member = $this->member($channelData);
                if ($member === null) {
                    $this->invalid($conn, $state);
                    return;
                }
                [$userId, $info] = $member;
                if (!$this->presence->has($channel, $userId) && $this->presence->count($channel) >= $this->config->maxPresenceMembers) {
                    $this->subscriptionError($conn, $channel, 'LimitReached', 'Presence channel is full');
                    return;
                }
            }
        }

        if ($revived !== null) {
            // Left and joined again before the daemon answered: the one request in flight serves the
            // new join, with the member this signature is for.
            $this->pending[$revived] = ['conn' => $conn, 'channel' => $channel, 'userId' => $userId, 'info' => $info, 'cancelled' => false];
            return;
        }
        $request = $this->link->subscribe($conn, $channel);
        $this->pending[$request] = ['conn' => $conn, 'channel' => $channel, 'userId' => $userId, 'info' => $info, 'cancelled' => false];
        $state->pending[$request] = true;
    }

    /**
     * The user a presence subscription is for, from the signed channel_data.
     *
     * @return array{0: string, 1: ?array}|null null if it is unusable
     */
    private function member(?string $channelData): ?array
    {
        if ($channelData === null || strlen($channelData) > $this->config->maxChannelDataBytes) {
            return null;
        }
        try {
            $data = json_decode($channelData, true, 8, JSON_THROW_ON_ERROR);
        } catch (\JsonException) {
            return null;
        }
        if (!is_array($data) || array_is_list($data)) {
            return null;
        }
        $id = $data['user_id'] ?? null;
        if (is_int($id) && $id >= 0) {
            $id = (string) $id;
        }
        if (!is_string($id) || $id === '' || strlen($id) > 128 || preg_match('/[\x00-\x1f\x7f]/', $id) === 1) {
            return null;
        }
        $info = $data['user_info'] ?? null;
        if ($info !== null && !is_array($info)) {
            return null;
        }
        // 1e999 decodes to INF, which cannot be sent back out to the other members.
        if ($info !== null) {
            try {
                json_encode($info, JSON_THROW_ON_ERROR);
            } catch (\JsonException) {
                return null;
            }
        }
        return [$id, $info];
    }

    /** The daemon put the connection in the room: now it is a member, and the browser is told. */
    private function joined(int $conn, ConnState $state, string $channel, ?string $userId, ?array $info): void
    {
        if ($userId === null) {
            $state->channels[$channel] = null;
            $this->joins++;
            $this->sendSucceeded($conn, $channel);
            return;
        }
        $result = $this->presence->add($channel, $conn, $userId, $info);
        if ($result === PresenceTable::FULL) {
            // Two users raced for the last seat and this one lost: leave the room again.
            $this->link->unsubscribe($conn, $channel);
            $this->subscriptionError($conn, $channel, 'LimitReached', 'Presence channel is full');
            return;
        }
        $state->channels[$channel] = $userId;
        $this->joins++;
        if ($result === PresenceTable::ADDED && $this->presence->count($channel) > 1) {
            $member = ['user_id' => $userId];
            if ($info !== null) {
                $member['user_info'] = $info;
            }
            $this->link->publish($channel, Frame::encode('pusher_internal:member_added', $member, $channel), $conn);
        }
        $this->sendSucceeded($conn, $channel);
    }

    private function sendSucceeded(int $conn, string $channel): void
    {
        $data = new \stdClass();
        if (ChannelName::kind($channel) === ChannelKind::Presence) {
            $summary = $this->presence->summary($channel);
            $hash = [];
            foreach ($summary['hash'] as $id => $info) {
                $hash[(string) $id] = $info === [] ? new \stdClass() : $info;
            }
            $data = ['presence' => ['count' => $summary['count'], 'ids' => $summary['ids'], 'hash' => (object) $hash]];
        }
        $this->link->send($conn, Frame::encode('pusher_internal:subscription_succeeded', $data, $channel));
    }

    // ---- pusher:unsubscribe, and leaving ------------------------------------------------------

    private function unsubscribe(int $conn, ConnState $state, Frame $frame): void
    {
        $channel = ($frame->object() ?? [])['channel'] ?? null;
        if (!is_string($channel)) {
            $this->invalid($conn, $state);
            return;
        }
        if (!array_key_exists($channel, $state->channels)) {
            // A join still waiting for the daemon (pusher-js waits for the success before it unsubscribes,
            // but another client may not): mark it, and status() takes the connection out of the room
            // again when the answer comes.
            foreach (array_keys($state->pending) as $request) {
                if ($this->pending[$request]['channel'] === $channel) {
                    $this->pending[$request]['cancelled'] = true;
                }
            }
            return;   // nothing else to leave; Pusher sends no answer either way
        }
        $userId = $state->channels[$channel];
        unset($state->channels[$channel]);
        $this->link->unsubscribe($conn, $channel);
        if ($userId !== null) {
            $this->memberLeft($channel, $conn, $userId);
        }
    }

    /** Remove a connection from the member list and tell the others if its user is gone. */
    private function memberLeft(string $channel, int $conn, string $userId): void
    {
        $result = $this->presence->remove($channel, $conn);
        if (is_string($result)) {
            $this->link->publish($channel, Frame::encode('pusher_internal:member_removed', ['user_id' => $userId], $channel), $conn);
        }
    }

    // ---- client events (whispers) -------------------------------------------------------------

    private function whisper(int $conn, ConnState $state, Frame $frame): void
    {
        if (!$this->config->clientEvents) {
            $this->error($conn, self::ERR_CLIENT_EVENTS, 'The app does not have client messaging enabled.');
            return;
        }
        $channel = $frame->channel;
        $data = $frame->relayData();
        if ($channel === null || $data === null || strlen($frame->event) <= strlen('client-')) {
            $this->invalid($conn, $state);
            return;
        }
        if (EncryptedChannels::isEncrypted($channel)) {
            // pusher-js refuses client events on an encrypted channel too: its receivers expect {nonce, ciphertext}.
            $this->error($conn, self::ERR_CLIENT_EVENTS, 'Client events are not supported on encrypted channels.');
            return;
        }
        if (!array_key_exists($channel, $state->channels) || ChannelName::kind($channel) === ChannelKind::Public) {
            $this->error($conn, self::ERR_UNAUTHORIZED, 'The client is not a member of the specified channel.');
            return;
        }
        if ($this->overLimit($state->whisperWindow, $state->whisperCount, $this->config->clientEventsPerSecond)) {
            // Refused, not punished (Reverb answers a client over its rate with this error alone): a typing
            // indicator that an application does not throttle must not cost its user the connection. A real
            // flood is ended by the limiter on all frames, which does strike.
            $this->rateLimited++;
            $this->error($conn, self::ERR_CLIENT_EVENTS, 'Rate limit exceeded');
            return;
        }
        // The user id is the signed one the connection joined with, never one the browser sent.
        $this->link->publish($channel, Frame::relay($frame->event, $channel, $data, $state->channels[$channel]), $conn);
    }

    // ---- errors, strikes and rates ------------------------------------------------------------

    private function error(int $conn, int $code, string $message): void
    {
        if ($code === self::ERR_UNAUTHORIZED) {
            $this->unauthorized++;
        }
        $this->link->send($conn, Frame::encode('pusher:error', ['code' => $code, 'message' => $message]));
    }

    private function subscriptionError(int $conn, string $channel, string $type, string $message): void
    {
        $this->subscriptionErrors++;
        $this->link->send($conn, Frame::encode('pusher:subscription_error', [
            'type' => $type, 'error' => $message, 'status' => self::SUBSCRIPTION_ERROR,
        ], $channel));
    }

    private function invalid(int $conn, ConnState $state): void
    {
        $this->badFrames++;
        $this->error($conn, self::ERR_BAD_FRAME, 'Invalid message format');
        $this->strike($conn, $state);
    }

    /**
     * Count one violation, and close the connection when it has too many *recent* ones (ADR-0042 A2). A
     * strike is forgiven for each full STRIKE_FORGIVE_SECONDS of quiet before the next one, so a
     * connection may break a rule once a minute for as long as it likes; faster than that the strikes
     * pile up and at max_strikes it is closed. Closing is for sustained abuse, not for a tab that has
     * been open for a week. Whole strikes and whole minutes: a burst is counted exactly.
     */
    private function strike(int $conn, ConnState $state): void
    {
        $now = ($this->clock)();
        $quiet = $now - $state->strikeStamp;   // negative if a clock went back: no forgiveness then
        if ($quiet >= self::STRIKE_FORGIVE_SECONDS) {
            $state->strikes = max(0, $state->strikes - (int) floor($quiet / self::STRIKE_FORGIVE_SECONDS));
        }
        $state->strikes++;
        $state->strikeStamp = $now;
        if ($state->strikes >= $this->config->maxStrikes && !$state->closing) {
            $state->closing = true;
            $this->strikeCloses++;
            $this->link->close($conn, self::CLOSE_POLICY, 'protocol violations');
        }
    }

    /**
     * Take one frame from the connection's bucket, which holds up to messageBurst and gets messagesPerSecond
     * back each second. A page that has just connected sends all its subscribes at once (the burst); one that
     * keeps sending faster than the rate empties the bucket, is answered 4301 and collects strikes.
     */
    private function takeMessageToken(ConnState $state): bool
    {
        $now = ($this->clock)();
        $state->messageTokens = min(
            (float) $this->config->messageBurst,
            $state->messageTokens + max(0.0, $now - $state->messageStamp) * $this->config->messagesPerSecond,
        );
        $state->messageStamp = $now;
        if ($state->messageTokens < 1.0) {
            return false;
        }
        $state->messageTokens -= 1.0;
        return true;
    }

    /** Count one event in the current second; true if that is more than $limit. */
    private function overLimit(int &$window, int &$count, int $limit): bool
    {
        $now = (int) floor(($this->clock)());
        if ($window !== $now) {
            $window = $now;
            $count = 0;
        }
        return ++$count > $limit;
    }
}
