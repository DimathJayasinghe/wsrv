<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

use Wsrv\WsrvException;

/** A frame from a browser that the gateway will not act on. The message is for a log, never for the peer. */
final class BadFrame extends WsrvException
{
}
