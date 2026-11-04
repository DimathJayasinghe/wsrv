<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

use Wsrv\Ipc\HandshakeRequest;

/**
 * A browser's upgrade that waits for this application's answer (ADR-0018). Answer exactly once,
 * within the daemon's `handshake_decision_timeout_ms` (default 3 s): after that the browser has
 * already been told 503 and the daemon answers NO_SUCH_CONN.
 *
 * It holds only the connection id, like Connection. The id becomes the id of the connection if you
 * accept: the `open` event follows with the same number.
 */
final class Upgrade
{
    private bool $answered = false;

    public function __construct(
        private readonly Client $client,
        public readonly int $conn,
        public readonly HandshakeRequest $request,
    ) {
    }

    /**
     * Let the browser in. $subprotocol must be one it offered.
     *
     * @return int the request id; the STATUS answer comes later
     * @throws \InvalidArgumentException for a name the client did not offer (nothing is sent; you may still answer)
     * @throws \LogicException if this upgrade was answered already
     */
    public function accept(?string $subprotocol = null): int
    {
        $this->assertOpen();
        if ($subprotocol !== null && $subprotocol !== '' && !in_array($subprotocol, $this->request->offered, true)) {
            throw new \InvalidArgumentException("the client did not offer the subprotocol '$subprotocol'");
        }
        $request = $this->client->acceptHandshake($this->conn, $subprotocol ?? '');
        $this->answered = true;
        return $request;
    }

    /**
     * Refuse the upgrade with one of 400, 401, 403, 404, 429, 500, 503. The reason text is fixed by
     * the daemon; nothing you pass reaches the browser.
     *
     * @return int the request id
     * @throws \LogicException if this upgrade was answered already
     */
    public function reject(int $status = 403): int
    {
        $this->assertOpen();
        $request = $this->client->rejectHandshake($this->conn, $status);
        $this->answered = true;
        return $request;
    }

    public function answered(): bool
    {
        return $this->answered;
    }

    private function assertOpen(): void
    {
        if ($this->answered) {
            throw new \LogicException('this upgrade was already answered');
        }
    }
}
