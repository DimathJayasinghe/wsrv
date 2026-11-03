<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

final class HelloOk
{
    public function __construct(
        public readonly int $major,
        public readonly int $minor,
        public readonly int $maxFrame,
    ) {
    }
}
