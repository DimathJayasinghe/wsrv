<?php
// SPDX-License-Identifier: Apache-2.0
// The REAL pusher-js and laravel-echo, in a real browser, against the gateway and a real wsrvd (ADR-0042
// interop gate). Manual: it needs the two npm packages and a browser, so CI does not run it.
//
//   npm pack pusher-js laravel-echo      # in an empty directory; unpack both tarballs
//   php tests/echo_browser/run.php /path/to/wsrvd /path/to/pusher-with-encryption.min.js /path/to/echo.iife.js
//
// It prints the address of a test page; open it in a browser, and it runs the scenario and reports
// the result here. Nothing leaves the machine: everything listens on 127.0.0.1.

declare(strict_types=1);

require __DIR__ . '/../bootstrap.php';
require __DIR__ . '/../wsrvd_process.php';

spl_autoload_register(static function (string $class): void {
    if (str_starts_with($class, 'Wsrv\\')) {
        $file = __DIR__ . '/../../src/' . str_replace('\\', '/', substr($class, 5)) . '.php';
        is_file($file) && require $file;
    }
});

use Wsrv\Client;
use Wsrv\Pusher\Gateway;
use Wsrv\Pusher\GatewayConfig;
use Wsrv\Worker;

[$daemonPath, $pusherJs, $echoJs] = [$argv[1] ?? '', $argv[2] ?? '', $argv[3] ?? ''];
(is_file($daemonPath) && is_file($pusherJs) && is_file($echoJs)) || fail('usage: php tests/echo_browser/run.php WSRVD PUSHER_JS ECHO_JS');

$dir = sys_get_temp_dir() . '/wsrv-echo-' . bin2hex(random_bytes(4));
mkdir($dir, 0700) || fail('mkdir');
$tokenFile = "$dir/publish.token";
exec(escapeshellarg($daemonPath) . ' --generate-token=' . escapeshellarg($tokenFile) . ' 2>&1', $out, $code);
$code === 0 || fail('--generate-token failed');

$key = 'echo-browser-key';
$secret = 'echo-browser-secret-0123456789ab';
$pagePort = random_int(20000, 40000);
$origin = "http://127.0.0.1:$pagePort";
$daemon = start_wsrvd($daemonPath, $dir, 'ipc', $tokenFile, ["--allow-origin=$origin"]);
$worker = new Worker(Client::connect($daemon['socket'], '', 5.0));
Gateway::forWorker(new GatewayConfig($key, $secret), $worker);

$resultFile = "$dir/result.json";
$restartFlag = "$dir/restart.flag";
$restartedFlag = "$dir/restarted.flag";
$server = proc_open(
    [PHP_BINARY, '-S', "127.0.0.1:$pagePort", __DIR__ . '/router.php'],
    [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', "$dir/php-server.log", 'w']],
    $pipes,
    null,
    [
        'PHP_CLI_SERVER_WORKERS' => '4', 'PATH' => (string) getenv('PATH'),
        'WSRV_KEY' => $key, 'WSRV_SECRET' => $secret,
        'WSRV_ENC_KEY' => base64_encode(random_bytes(32)),
        'WSRV_PUBLISH_URL' => 'http://127.0.0.1:' . $daemon['publishPort'], 'WSRV_TOKEN_FILE' => $tokenFile,
        'WSRV_DAEMON_PORT' => (string) $daemon['port'],
        'WSRV_PUSHER_JS' => $pusherJs, 'WSRV_ECHO_JS' => $echoJs, 'WSRV_RESULT_FILE' => $resultFile,
        'WSRV_RESTART_FLAG' => $restartFlag, 'WSRV_RESTARTED_FLAG' => $restartedFlag,
    ]
) ?: fail('cannot start the php server');

echo "open this page in a browser: $origin/\n";
$deadline = microtime(true) + (float) (getenv('WSRV_ECHO_TIMEOUT') ?: 180);
while (!is_file($resultFile) && microtime(true) < $deadline) {
    $worker->step(0.1);
    if (is_file($restartFlag)) {
        // Stop the daemon (it closes every browser), then start a new daemon on the same ports with a
        // new worker: what a deploy does. The browsers must reconnect and resubscribe by themselves.
        unlink($restartFlag);
        proc_terminate($daemon['process'], SIGTERM);
        $stopBy = microtime(true) + 10;
        while (proc_get_status($daemon['process'])['running'] && microtime(true) < $stopBy) {
            usleep(50000);
        }
        $daemon = start_wsrvd($daemonPath, $dir, 'ipc', $tokenFile, [
            "--allow-origin=$origin", '--listen-port=' . $daemon['port'], '--publish-port=' . $daemon['publishPort'],
        ]);
        $worker = new Worker(Client::connect($daemon['socket'], '', 5.0));
        Gateway::forWorker(new GatewayConfig($key, $secret), $worker);
        touch($restartedFlag);
    }
}
$results = is_file($resultFile) ? json_decode((string) file_get_contents($resultFile), true) : null;
proc_terminate($server);
proc_terminate($daemon['process'], SIGTERM);
$results !== null || fail('the page did not report in time');
$failed = 0;
foreach ($results as $r) {
    echo ($r['ok'] ? 'ok   ' : 'FAIL ') . $r['name'] . ($r['ok'] ? '' : ': ' . $r['error']) . "\n";
    $failed += $r['ok'] ? 0 : 1;
}
echo $failed === 0 ? "all browser checks passed\n" : "$failed browser check(s) failed\n";
exit($failed === 0 ? 0 : 1);
