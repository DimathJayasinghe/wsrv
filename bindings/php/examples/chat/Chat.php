<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Examples;

use Wsrv\ConnectionClosed;
use Wsrv\Embedded\Connection;

/**
 * Chat rooms over the embedded server, in the text protocol of examples/chat (C++): one command per
 * text message,
 *
 *   join <room>          ok joined <room>
 *   leave <room>         ok left <room>
 *   say <room> <text>    ok sent <room> <accepted> <refused>    (the others get `msg <room> user<id>: <text>`)
 *   members <room>       ok members <room> <n>
 *   rooms                ok rooms <a> <b> ...
 *
 * Any failure is `error: <why>`. The C++ chat gets its bounds from the broker; PHP has no broker yet,
 * so THIS class owns them, and every one is a constant below. Nothing the peer sends sizes anything
 * unbounded: a name is at most MAX_ROOM_NAME bytes of [A-Za-z0-9._-], a connection is in at most
 * MAX_ROOMS_PER_CONNECTION rooms, the server holds at most MAX_ROOMS rooms, a message is at most
 * MAX_TEXT bytes. Text is valid UTF-8 by the time it gets here (the library closes the connection with 1007
 * otherwise), so there is no check for it. Policy, not safety: you may only speak in or ask about a room you joined.
 */
final class Chat
{
    public const MAX_ROOM_NAME = 64;
    public const MAX_ROOMS_PER_CONNECTION = 16;
    public const MAX_ROOMS = 1024;
    public const MAX_TEXT = 4096;

    /** @var array<string, array<int, Connection>> room => (connection id => connection) */
    private array $rooms = [];
    /** @var array<int, array<string, true>> connection id => the rooms it joined */
    private array $joined = [];

    /** A text message from a client: one command. */
    public function onMessage(Connection $c, string $line, bool $binary): void
    {
        if ($binary) {
            $this->reply($c, 'error: text commands only');
            return;
        }
        [$word, $rest] = self::split($line);
        match ($word) {
            'rooms' => $rest === '' ? $this->listRooms($c) : $this->reply($c, 'error: rooms takes no argument'),
            'join' => $this->withRoom($c, $rest, $this->join(...)),
            'leave' => $this->withRoom($c, $rest, $this->leave(...)),
            'members' => $this->withRoom($c, $rest, $this->members(...)),
            'say' => $this->say($c, $rest),
            default => $this->reply($c, 'error: unknown command (join, leave, say, members, rooms)'),
        };
    }

    /** The connection ended, however it ended: give every room back. */
    public function onClose(Connection $c): void
    {
        foreach (array_keys($this->joined[$c->id] ?? []) as $room) {
            $this->removeFrom($room, $c->id);
        }
        unset($this->joined[$c->id]);
    }

    /** Rooms that exist right now (tests and metrics). */
    public function roomCount(): int
    {
        return count($this->rooms);
    }

    // ---- commands ----------------------------------------------------------------------------

    private function join(Connection $c, string $room): void
    {
        if (isset($this->joined[$c->id][$room])) {
            $this->reply($c, "error: already in $room");
            return;
        }
        if (count($this->joined[$c->id] ?? []) >= self::MAX_ROOMS_PER_CONNECTION) {
            $this->reply($c, 'error: too many rooms (at most ' . self::MAX_ROOMS_PER_CONNECTION . ')');
            return;
        }
        if (!isset($this->rooms[$room]) && count($this->rooms) >= self::MAX_ROOMS) {
            $this->reply($c, 'error: the server has too many rooms');
            return;
        }
        $this->rooms[$room][$c->id] = $c;
        $this->joined[$c->id][$room] = true;
        $this->reply($c, "ok joined $room");
    }

    private function leave(Connection $c, string $room): void
    {
        if (!isset($this->joined[$c->id][$room])) {
            $this->reply($c, "error: not in $room");
            return;
        }
        unset($this->joined[$c->id][$room]);
        $this->removeFrom($room, $c->id);
        $this->reply($c, "ok left $room");
    }

    private function members(Connection $c, string $room): void
    {
        // Only for rooms you are in: an open `members` would let anyone probe which rooms exist.
        if (!isset($this->joined[$c->id][$room])) {
            $this->reply($c, "error: not in $room");
            return;
        }
        $this->reply($c, "ok members $room " . count($this->rooms[$room]));
    }

    private function listRooms(Connection $c): void
    {
        $names = array_keys($this->joined[$c->id] ?? []);
        sort($names);
        $this->reply($c, rtrim('ok rooms ' . implode(' ', $names)));
    }

    private function say(Connection $c, string $rest): void
    {
        [$room, $text] = self::split($rest);
        if ($room === '' || $text === '') {
            $this->reply($c, 'error: usage: say <room> <text>');
            return;
        }
        if (strlen($text) > self::MAX_TEXT) {
            $this->reply($c, 'error: message too long (at most ' . self::MAX_TEXT . ' bytes)');
            return;
        }
        if (!isset($this->joined[$c->id][$room])) {
            $this->reply($c, "error: not in $room");  // you may only speak where you joined
            return;
        }
        $accepted = 0;
        $refused = 0;
        foreach ($this->rooms[$room] as $id => $member) {
            if ($id === $c->id) {
                continue;  // the sender does not hear itself
            }
            try {
                $sent = $member->send("msg $room user{$c->id}: $text");
            } catch (ConnectionClosed) {
                $sent = false;  // ended since the last event: its close event cleans up
            }
            $sent ? $accepted++ : $refused++;
        }
        // The refused count is the honest answer to a full queue: the sender can see load being shed.
        $this->reply($c, "ok sent $room $accepted $refused");
    }

    // ---- helpers -----------------------------------------------------------------------------

    /** @param callable(Connection, string): void $action */
    private function withRoom(Connection $c, string $room, callable $action): void
    {
        if ($room === '' || str_contains($room, ' ')) {
            $this->reply($c, 'error: expected one room name');
            return;
        }
        if (strlen($room) > self::MAX_ROOM_NAME || preg_match('/^[A-Za-z0-9._-]+$/', $room) !== 1) {
            $this->reply($c, 'error: a room name is 1 to ' . self::MAX_ROOM_NAME . ' characters of A-Z a-z 0-9 . _ -');
            return;
        }
        $action($c, $room);
    }

    private function removeFrom(string $room, int $id): void
    {
        unset($this->rooms[$room][$id]);
        if (($this->rooms[$room] ?? []) === []) {
            unset($this->rooms[$room]);  // an empty room costs nothing
        }
    }

    private function reply(Connection $c, string $text): void
    {
        try {
            $c->send($text);
        } catch (ConnectionClosed) {
            // gone already; its close event follows
        }
    }

    /** @return array{0: string, 1: string} the word before the first space, and the rest */
    private static function split(string $line): array
    {
        $space = strpos($line, ' ');
        return $space === false ? [$line, ''] : [substr($line, 0, $space), substr($line, $space + 1)];
    }
}
