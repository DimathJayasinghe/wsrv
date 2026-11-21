<?php
// SPDX-License-Identifier: Apache-2.0
// Refresh src/Embedded/wsrv.cdef from include/wsrv/wsrv.h:  php bindings/php/tools/gen_cdef.php
// The test `cdef: src/Embedded/wsrv.cdef is exactly what the generator makes` fails when it is stale.

declare(strict_types=1);

require __DIR__ . '/CdefGenerator.php';

$header = dirname(__DIR__, 3) . '/include/wsrv/wsrv.h';
$target = dirname(__DIR__) . '/src/Embedded/wsrv.cdef';
$text = file_get_contents($header);
if ($text === false) {
    fwrite(STDERR, "cannot read $header\n");
    exit(1);
}
file_put_contents($target, Wsrv\Tools\CdefGenerator::fromHeader($text));
echo "wrote $target\n";
