<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

/** The daemon's answer to one command. OK means accepted for sending, not delivered. */
final class StatusReply
{
    public function __construct(
        public readonly int $request,
        public readonly int $status,
    ) {
    }
}
