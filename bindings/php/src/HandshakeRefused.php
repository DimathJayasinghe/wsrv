<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * The daemon answered HELLO with a STATUS instead of HELLO_OK. The reason is deliberately vague on
 * the wire (wrong token, wrong version and "not a HELLO" look the same); BACKPRESSURE means another
 * application is already connected.
 */
final class HandshakeRefused extends WsrvException
{
    public function __construct(public readonly int $status)
    {
        parent::__construct('wsrvd refused the handshake (status ' . $status . ' = ' . StatusCode::name($status) . ')');
    }
}
