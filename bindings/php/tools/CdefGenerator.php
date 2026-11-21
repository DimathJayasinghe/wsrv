<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Tools;

/**
 * Turns include/wsrv/wsrv.h into the string FFI::cdef() accepts (ADR-0025 section 2).
 *
 * FFI has no preprocessor, so what it cannot read is removed and nothing else is touched: the
 * `extern "C"` guards, the preprocessor lines, the WSRV_API export marker and the comments. The
 * declarations themselves (structs, enums, functions) stay byte for byte, so the cdef cannot say
 * something the header does not.
 */
final class CdefGenerator
{
    public static function fromHeader(string $header): string
    {
        // The C++ guard pairs first: their braces would survive the line filter below.
        $header = preg_replace('/#ifdef __cplusplus\s*extern "C" \{\s*#endif/', '', $header)
            ?? throw new \RuntimeException('regex failed (open guard)');
        $header = preg_replace('/#ifdef __cplusplus\s*\}\s*#endif/', '', $header)
            ?? throw new \RuntimeException('regex failed (close guard)');
        $header = preg_replace('~/\*.*?\*/~s', '', $header)
            ?? throw new \RuntimeException('regex failed (comments)');
        $header = preg_replace('/^[ \t]*#.*$/m', '', $header)
            ?? throw new \RuntimeException('regex failed (preprocessor)');
        $header = preg_replace('/\bWSRV_API[ \t]+/', '', $header)
            ?? throw new \RuntimeException('regex failed (export marker)');

        $lines = [];
        foreach (explode("\n", $header) as $line) {
            $line = rtrim($line);
            if ($line !== '') {
                $lines[] = $line;
            }
        }
        return implode("\n", $lines) . "\n";
    }
}
