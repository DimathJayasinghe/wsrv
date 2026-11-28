<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * A message is larger than one IPC frame the daemon accepts (the `max_frame` of its HELLO_OK), so it
 * was refused here and nothing was written.
 *
 * Unlike the other ProtocolErrors the link is fine: the stream is still in step and the caller may
 * carry on, so the Supervisor reports this like an exception from a handler and keeps serving
 * instead of dropping the daemon connection (and with it every browser). It extends ProtocolError
 * because 1.0 threw ProtocolError here, and a `catch (ProtocolError)` written for that must keep
 * catching it.
 */
final class MessageTooLarge extends ProtocolError
{
}
