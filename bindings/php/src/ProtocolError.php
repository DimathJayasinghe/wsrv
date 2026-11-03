<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * The other side sent bytes that are not valid IPC protocol v0 (or we were asked to send something
 * the protocol cannot express). A length-prefixed stream cannot be resynchronised, so after this
 * the connection must be dropped.
 */
final class ProtocolError extends WsrvException
{
}
