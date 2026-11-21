<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Embedded;

use Wsrv\Ipc\HandshakeRequest;

/**
 * A browser's upgrade that waits for this application's answer (ADR-0018, ADR-0025 A1). Answer
 * exactly once, within `handshake_decision_timeout_ms` (default 3 s): after that the library has
 * already told the browser 503 and answering throws ConnectionClosed.
 *
 * It holds only the connection id. The id becomes the id of the connection if you accept: the `open`
 * event follows with the same number. Same methods as the sidecar's Wsrv\Upgrade.
 */
final class Upgrade
{
    private bool $answered = false;

    public function __construct(
        private readonly Server $server,
        public readonly int $conn,
        public readonly HandshakeRequest $request,
    ) {
    }

    /**
     * Let the browser in. $subprotocol must be one it offered.
     *
     * @throws \InvalidArgumentException for a name the client did not offer (nothing changes; you may still answer)
     * @throws \LogicException if this upgrade was answered already
     * @throws \Wsrv\ConnectionClosed the library no longer waits for an answer (timeout, or the client left)
     */
    public function accept(?string $subprotocol = null): void
    {
        $this->assertOpen();
        if ($subprotocol !== null && $subprotocol !== '' && !in_array($subprotocol, $this->request->offered, true)) {
            throw new \InvalidArgumentException("the client did not offer the subprotocol '$subprotocol'");
        }
        $this->server->acceptHandshake($this->conn, $subprotocol ?? '');
        $this->answered = true;
    }

    /**
     * Refuse the upgrade with one of 400, 401, 403, 404, 429, 500, 503. The reason text is fixed by
     * the library; nothing you pass reaches the browser.
     *
     * @throws \InvalidArgumentException for any other status (nothing changes; you may still answer)
     * @throws \LogicException if this upgrade was answered already
     * @throws \Wsrv\ConnectionClosed the library no longer waits for an answer
     */
    public function reject(int $status = 403): void
    {
        $this->assertOpen();
        $this->server->rejectHandshake($this->conn, $status);
        $this->answered = true;
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
