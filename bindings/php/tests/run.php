<?php
// SPDX-License-Identifier: Apache-2.0
// Run with: php tests/run.php [filter]

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

foreach (['CodecTest', 'FrameDecoderTest', 'ClientTest', 'WorkerTest'] as $file) {
    require __DIR__ . '/' . $file . '.php';
}

$filter = $argv[1] ?? '';
$passed = 0;
$failed = [];
foreach ($GLOBALS['wsrv_tests'] as $name => $body) {
    if ($filter !== '' && !str_contains($name, $filter)) {
        continue;
    }
    try {
        $body();
        $passed++;
    } catch (Throwable $e) {
        $failed[$name] = $e;
    }
}
foreach ($failed as $name => $e) {
    fwrite(STDERR, "FAIL $name\n    " . get_class($e) . ': ' . $e->getMessage() . "\n    at " . $e->getFile() . ':' . $e->getLine() . "\n");
}
echo $passed . ' passed, ' . count($failed) . " failed\n";
exit($failed === [] ? 0 : 1);
