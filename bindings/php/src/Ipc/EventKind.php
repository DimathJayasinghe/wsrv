<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

/** Same five kinds, same numbers, as wsrv_event_type. */
enum EventKind: int
{
    case Open = 1;
    case Text = 2;
    case Binary = 3;
    case Close = 4;
    case Error = 5;
}
