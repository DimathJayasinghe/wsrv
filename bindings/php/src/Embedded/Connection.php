<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Embedded;

/**
 * A handle on one WebSocket connection of an embedded Server, valid from its `open` event until its
 * `close` event. It holds only the id: using it after `close` throws ConnectionClosed (the library
 * answers NO_SUCH_CONN), it never touches freed memory.
 */
final class Connection
{
    public function __construct(
        private readonly Server $server,
        public readonly int $id,
        public readonly string $peer = '',
    ) {
    }

    /**
     * Send a text message (must be valid UTF-8, else InvalidArgumentException and nothing is sent).
     *
     * @return bool true when accepted; false when a queue is full (WSRV_ERR_BACKPRESSURE): the
     *              message was NOT queued, try again later or close the connection
     * @throws \Wsrv\ConnectionClosed the connection has ended
     */
    public function send(string $text): bool
    {
        return $this->server->sendText($this->id, $text);
    }

    /** @return bool as send() @throws \Wsrv\ConnectionClosed */
    public function sendBinary(string $data): bool
    {
        return $this->server->sendBinary($this->id, $data);
    }

    /**
     * Begin the closing handshake. `$code` must be sendable (1000-1003, 1007-1014, 3000-4999); the
     * reason is cut to 123 bytes by the library.
     *
     * @throws \Wsrv\ConnectionClosed the connection has ended
     */
    public function close(int $code = 1000, string $reason = ''): void
    {
        $this->server->closeConnection($this->id, $code, $reason);
    }
}
