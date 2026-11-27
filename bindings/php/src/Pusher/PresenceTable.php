<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * Who is in each presence channel, for the connections of one daemon.
 *
 * A member is a user, not a connection: a person with two tabs open is one member with two
 * connections, and leaves when the last tab closes. The table lives in the worker's memory; when the
 * worker goes away the daemon closes its connections, so the table never outlives what it describes.
 * The number of members of one channel is bounded.
 */
final class PresenceTable
{
    public const ADDED = 0;       // a new member: tell the others
    public const SAME_USER = 1;   // another connection of an existing member: nothing to tell
    public const FULL = 2;        // refused: the channel has the maximum number of members
    public const STAYS = -1;      // the connection left, the user still has another one
    public const ABSENT = -2;     // nothing to remove

    /** @var array<string, array{users: array<string, array{info: ?array, conns: array<int, true>}>, by_conn: array<int, string>}> */
    private array $channels = [];

    public function __construct(private readonly int $maxMembers)
    {
        if ($maxMembers < 1) {
            throw new \InvalidArgumentException('maxMembers must be at least 1');
        }
    }

    public function add(string $channel, int $conn, string $userId, ?array $info): int
    {
        $ch = &$this->channels[$channel];
        $ch ??= ['users' => [], 'by_conn' => []];
        if (isset($ch['by_conn'][$conn])) {
            return self::SAME_USER;   // subscribing twice changes nothing
        }
        if (isset($ch['users'][$userId])) {
            $ch['users'][$userId]['conns'][$conn] = true;
            $ch['by_conn'][$conn] = $userId;
            return self::SAME_USER;
        }
        if (count($ch['users']) >= $this->maxMembers) {
            if ($ch['users'] === []) {
                unset($this->channels[$channel]);
            }
            return self::FULL;
        }
        $ch['users'][$userId] = ['info' => $info, 'conns' => [$conn => true]];
        $ch['by_conn'][$conn] = $userId;
        return self::ADDED;
    }

    /** @return string|int the user id if this was their last connection, else STAYS or ABSENT */
    public function remove(string $channel, int $conn): string|int
    {
        $ch = &$this->channels[$channel];
        if ($ch === null || !isset($ch['by_conn'][$conn])) {
            unset($this->channels[$channel]);
            return self::ABSENT;
        }
        $userId = (string) $ch['by_conn'][$conn];
        unset($ch['by_conn'][$conn], $ch['users'][$userId]['conns'][$conn]);
        if ($ch['users'][$userId]['conns'] !== []) {
            return self::STAYS;
        }
        unset($ch['users'][$userId]);
        if ($ch['users'] === []) {
            unset($this->channels[$channel]);
        }
        return $userId;
    }

    public function has(string $channel, string $userId): bool
    {
        return isset($this->channels[$channel]['users'][$userId]);
    }

    public function count(string $channel): int
    {
        return count($this->channels[$channel]['users'] ?? []);
    }

    /** Channels that have members. */
    public function channels(): int
    {
        return count($this->channels);
    }

    /** Users in all channels together (a user in two channels counts twice). */
    public function members(): int
    {
        $n = 0;
        foreach ($this->channels as $channel) {
            $n += count($channel['users']);
        }
        return $n;
    }

    /**
     * The `presence` object of a subscription_succeeded frame.
     *
     * @return array{count: int, ids: list<string>, hash: array<string, array>}
     */
    public function summary(string $channel): array
    {
        $ids = [];
        $hash = [];
        foreach ($this->channels[$channel]['users'] ?? [] as $userId => $user) {
            $id = (string) $userId;   // PHP turns a numeric-looking array key into an int
            $ids[] = $id;
            $hash[$id] = $user['info'] ?? [];
        }
        return ['count' => count($ids), 'ids' => $ids, 'hash' => $hash];
    }
}
