<?php
// SPDX-License-Identifier: Apache-2.0
// Run with: php tests/run.php [filter]

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

foreach (['LintTest', 'CodecTest', 'FrameDecoderTest', 'ClientTest', 'WorkerTest', 'HandshakeTest', 'EmbeddedCdefTest'] as $file) {
    require __DIR__ . '/' . $file . '.php';
}

$filter = $argv[1] ?? '';
$passed = 0;
$failed = [];
$skipped = [];
foreach ($GLOBALS['wsrv_tests'] as $name => $body) {
    if ($filter !== '' && !str_contains($name, $filter)) {
        continue;
    }
    try {
        $body();
        $passed++;
    } catch (SkippedTest $e) {
        $skipped[$name] = $e->getMessage();
    } catch (Throwable $e) {
        $failed[$name] = $e;
    }
}
foreach ($skipped as $name => $why) {
    fwrite(STDERR, "SKIP $name: $why\n");
}
foreach ($failed as $name => $e) {
    fwrite(STDERR, "FAIL $name\n    " . get_class($e) . ': ' . $e->getMessage() . "\n    at " . $e->getFile() . ':' . $e->getLine() . "\n");
}
echo $passed . ' passed, ' . count($failed) . ' failed, ' . count($skipped) . " skipped\n";
// Under CTest a skipped test means part of the product went unchecked: that is a failure there.
$strict = getenv('WSRV_PHP_NO_SKIP') === '1';
exit($failed === [] && (!$strict || $skipped === []) ? 0 : 1);
