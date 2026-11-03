<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/** The daemon closed the socket (shutdown, stall timeout, or a protocol error on our side). */
final class ConnectionClosed extends WsrvException
{
}
