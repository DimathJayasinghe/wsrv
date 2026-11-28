<?php
// SPDX-License-Identifier: Apache-2.0
// Starts a real wsrvd for the end-to-end tests that need the broker and a publish endpoint.
// Needs bootstrap.php (fail()).

declare(strict_types=1);

/**
 * Start a wsrvd with the broker and a publish endpoint; returns its process, ports and sockets.
 *
 * @param list<string> $extra more daemon arguments
 * @return array{process: resource, port: int, publishPort: int, socket: string}
 */
function start_wsrvd(string $daemonPath, string $dir, string $name, string $tokenFile, array $extra = []): array
{
    $socket = "$dir/$name.sock";
    $process = proc_open(
        array_merge([$daemonPath, "--ipc-socket=$socket", '--listen-port=0', '--allow-origin=https://app.example.com',
            '--shutdown-grace-ms=1500', '--broker=true', '--publish-port=0', "--publish-token-file=$tokenFile",
            '--max-message-size=16384'], $extra),
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['pipe', 'w']],
        $pipes
    ) ?: fail('cannot start wsrvd');
    $port = 0;
    $publishPort = 0;
    $log = '';
    $deadline = microtime(true) + 10;
    while (($port === 0 || $publishPort === 0) && microtime(true) < $deadline) {
        $r = [$pipes[2]];
        $w = $e = null;
        if (stream_select($r, $w, $e, 0, 200000) > 0) {
            $line = fgets($pipes[2]);
            $log .= (string) $line;
            if ($line !== false && preg_match('/event=listening .*listening on port (\d+)/', $line, $m)) {
                $port = (int) $m[1];
            }
            if ($line !== false && preg_match('/event=publish_listening .*:(\d+)"/', $line, $m)) {
                $publishPort = (int) $m[1];
            }
        }
    }
    ($port > 0 && $publishPort > 0) || fail("wsrvd did not start:\n$log");
    return ['process' => $process, 'port' => $port, 'publishPort' => $publishPort, 'socket' => $socket];
}
