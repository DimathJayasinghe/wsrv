<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

/**
 * One thing that happened on a WebSocket connection. `data` is the peer address for Open, the
 * message for Text/Binary, the close reason for Close and an error name for Error.
 * `closeCode` and `clean` mean something only for Close (1005 and 1006 can appear: the core reports
 * them, they never travel on the wire).
 */
final class Event
{
    public function __construct(
        public readonly EventKind $kind,
        public readonly int $conn,
        public readonly int $closeCode,
        public readonly bool $clean,
        public readonly string $data,
    ) {
    }
}
