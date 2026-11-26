<?php
// SPDX-License-Identifier: Apache-2.0
// A one-shot stand-in for wsrvd's TCP IPC listener, for ClientTest. Run as a child process:
//   php tcp_fake_daemon.php TOKEN
// Prints "tcp://127.0.0.1:PORT" on its first line, accepts one connection, checks that the first
// frame is a HELLO carrying TOKEN, answers HELLO_OK (max frame 4096), prints what it saw and exits.
// Why a process and not fork: pcntl does not exist on Windows, and the client tests must run there.

declare(strict_types=1);

$token = $argv[1] ?? '';
$server = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
if ($server === false) {
    fwrite(STDERR, "listen failed: $error\n");
    exit(2);
}
echo 'tcp://' . stream_socket_get_name($server, false) . "\n";
fflush(STDOUT);

$peer = @stream_socket_accept($server, 10.0);
if ($peer === false) {
    fwrite(STDERR, "no connection\n");
    exit(3);
}
$length = fread($peer, 4);
$unpacked = $length === false || strlen($length) !== 4 ? false : unpack('N', $length);
$frame = $unpacked === false ? '' : (string) fread($peer, $unpacked[1]);
// HELLO: type 1, flags 0, major u16, minor u16, token length u16, token.
$matched = strlen($frame) >= 8 && ord($frame[0]) === 1 && substr($frame, 8) === $token;
fwrite($peer, pack('NCC', 10, 2, 0) . pack('nnN', 0, 1, 4096));
echo ($matched ? 'hello-ok-sent token-matched' : 'hello-ok-sent token-MISMATCH') . "\n";
fflush(STDOUT);
stream_set_timeout($peer, 3);
fread($peer, 1);  // until the client closes
