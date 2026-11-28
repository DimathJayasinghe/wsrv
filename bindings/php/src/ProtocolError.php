<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * The other side sent bytes that are not valid IPC protocol v0 (or we were asked to send something
 * the protocol cannot express). A length-prefixed stream cannot be resynchronised, so after bytes
 * from the daemon that fail to parse the connection must be dropped. A refusal to *send* happens
 * before anything is written and leaves the stream intact; MessageTooLarge is the subclass for the
 * one such case a running application meets.
 */
class ProtocolError extends WsrvException
{
}
