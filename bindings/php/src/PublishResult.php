<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/** What a publish did: `$accepted + $refused === $subscribers`. Accepted means handed to the subscriber's loop. */
final class PublishResult
{
    public function __construct(
        public readonly int $subscribers,
        public readonly int $accepted,
        public readonly int $refused,
    ) {
    }
}
