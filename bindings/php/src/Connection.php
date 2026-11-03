<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * A handle on one WebSocket connection, valid from its `open` event until its `close` event. It
 * holds only the id: every method is a command to the daemon, answered later by a StatusReply.
 * Using it after `close` is harmless (the daemon answers NO_SUCH_CONN).
 */
final class Connection
{
    public function __construct(
        private readonly Client $client,
        public readonly int $id,
        public readonly string $peer = '',
    ) {
    }

    /** Send a text message (must be valid UTF-8). @return int the request id */
    public function send(string $text): int
    {
        return $this->client->sendText($this->id, $text);
    }

    /** @return int the request id */
    public function sendBinary(string $data): int
    {
        return $this->client->sendBinary($this->id, $data);
    }

    /** Begin the closing handshake. @return int the request id */
    public function close(int $code = 1000, string $reason = ''): int
    {
        return $this->client->closeConnection($this->id, $code, $reason);
    }
}
