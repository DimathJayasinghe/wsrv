<?php
// SPDX-License-Identifier: Apache-2.0
// Run with: php tests/run.php [filter]

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

foreach (['LintTest', 'CodecTest', 'FrameDecoderTest', 'ClientTest', 'WorkerTest', 'HandshakeTest', 'PublisherTest', 'ClusterPublisherTest', 'SupervisorTest', 'EmbeddedCdefTest', 'InstallTest', 'PackageTest', 'PusherCoreTest', 'PusherGatewayTest', 'LaravelConfigTest'] as $file) {
    require __DIR__ . '/' . $file . '.php';
}

// A test that never ends (a loop waiting for something a regression stopped producing) must fail with a
// name, not hang the suite: each test gets TEST_SECONDS, then the alarm interrupts it with an exception.
const TEST_SECONDS = 30;
$watchdog = function_exists('pcntl_alarm') && function_exists('pcntl_async_signals') && defined('SIGALRM');
if ($watchdog) {
    pcntl_async_signals(true);
    pcntl_signal(SIGALRM, static function (): never {
        throw new RuntimeException('the test ran for more than ' . TEST_SECONDS . ' seconds: it hangs');
    });
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
        $watchdog && pcntl_alarm(TEST_SECONDS);
        $body();
        $watchdog && pcntl_alarm(0);
        $passed++;
    } catch (SkippedTest $e) {
        $watchdog && pcntl_alarm(0);
        $skipped[$name] = $e->getMessage();
    } catch (Throwable $e) {
        $watchdog && pcntl_alarm(0);
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
