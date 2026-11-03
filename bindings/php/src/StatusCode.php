<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/** The values of wsrv_status in include/wsrv/wsrv.h, as carried by STATUS frames. */
final class StatusCode
{
    public const OK = 0;
    public const TIMEOUT = 1;
    public const INVALID_ARGUMENT = 2;
    public const INVALID_CONFIG = 3;
    public const ADDRESS_IN_USE = 4;
    public const NO_SUCH_CONN = 5;
    public const BACKPRESSURE = 6;
    public const INVALID_UTF8 = 7;
    public const WRONG_PROCESS = 8;
    public const RESOURCE = 9;
    public const IO = 10;
    public const INTERNAL = 11;

    private const NAMES = [
        0 => 'OK', 1 => 'TIMEOUT', 2 => 'INVALID_ARGUMENT', 3 => 'INVALID_CONFIG',
        4 => 'ADDRESS_IN_USE', 5 => 'NO_SUCH_CONN', 6 => 'BACKPRESSURE', 7 => 'INVALID_UTF8',
        8 => 'WRONG_PROCESS', 9 => 'RESOURCE', 10 => 'IO', 11 => 'INTERNAL',
    ];

    public static function name(int $status): string
    {
        return self::NAMES[$status] ?? 'UNKNOWN(' . $status . ')';
    }
}
