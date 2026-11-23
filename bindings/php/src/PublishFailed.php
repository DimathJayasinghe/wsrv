<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * The publish endpoint said no (a status other than 200), or could not be reached. `$status` is the
 * HTTP status, or 0 when there was no answer at all (connection refused, timeout, a reply that is
 * not HTTP). 429 and 503 mean "try again later"; 401 is the wrong token; 400, 413 and 415 are
 * mistakes in the call and will fail the same way next time.
 */
final class PublishFailed extends WsrvException
{
    public function __construct(
        string $message,
        public readonly int $status = 0,
        public readonly string $body = '',
    ) {
        parent::__construct($message);
    }

    /** True when the same call may succeed later: the server was busy or shutting down, or unreachable. */
    public function isRetryable(): bool
    {
        return $this->status === 0 || $this->status === 429 || $this->status === 503;
    }
}
