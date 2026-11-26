<?php
// SPDX-License-Identifier: Apache-2.0
// A minimal WebSocket client (RFC 6455) shared by the interop tests. Needs bootstrap.php (fail()).

declare(strict_types=1);

// ---- a minimal WebSocket client (RFC 6455), enough to drive the daemon ----------------------

/**
 * Open a TCP connection, send an upgrade request and read the HTTP answer head.
 *
 * @return array{0: resource, 1: string, 2: string} [stream, response head, Sec-WebSocket-Key]
 */
function ws_request(int $port, string $extraHeaders = '', string $path = '/chat')
{
    $s = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5) ?: fail("connect: $error");
    $key = base64_encode(random_bytes(16));
    fwrite($s, "GET $path HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\nOrigin: https://app.example.com\r\n"
        . $extraHeaders . "\r\n");
    stream_set_timeout($s, 5);
    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $byte = fread($s, 1);
        if ($byte === false || $byte === '') {
            fail('no HTTP response: ' . $head);
        }
        $head .= $byte;
    }
    return [$s, $head, $key];
}

/** @return resource */
function ws_connect(int $port, string $extraHeaders = '', string $path = '/chat')
{
    [$s, $head, $key] = ws_request($port, $extraHeaders, $path);
    str_starts_with($head, 'HTTP/1.1 101') || fail('handshake refused: ' . $head);
    $accept = base64_encode(sha1($key . '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', true));
    str_contains($head, $accept) || fail('wrong Sec-WebSocket-Accept');
    return $s;
}

/**
 * Send an upgrade request but do not wait for the answer: the daemon holds it until the
 * application decides, and the test must run the worker meanwhile.
 *
 * @return array{0: resource, 1: string} [stream, key]
 */
function ws_request_async(int $port, string $extraHeaders = '', string $path = '/chat'): array
{
    $s = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5) ?: fail("connect: $error");
    $key = base64_encode(random_bytes(16));
    fwrite($s, "GET $path HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\nOrigin: https://app.example.com\r\n"
        . $extraHeaders . "\r\n");
    stream_set_timeout($s, 5);
    return [$s, $key];
}

/**
 * Read the HTTP answer head of a request sent with ws_request_async().
 *
 * @param array{0: resource, 1: string} $pending
 * @return array{0: resource, 1: string}
 */
function ws_finish(array $pending): array
{
    [$s] = $pending;
    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $byte = fread($s, 1);
        if ($byte === false || $byte === '') {
            fail('no HTTP response: ' . $head);
        }
        $head .= $byte;
    }
    return [$s, $head];
}

/** @param resource $s */
function ws_send($s, int $opcode, string $payload): void
{
    $length = strlen($payload);
    $head = chr(0x80 | $opcode);
    if ($length < 126) {
        $head .= chr(0x80 | $length);
    } elseif ($length < 65536) {
        $head .= chr(0x80 | 126) . pack('n', $length);
    } else {
        $head .= chr(0x80 | 127) . pack('J', $length);
    }
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0; $i < $length; $i++) {
        $masked .= $payload[$i] ^ $mask[$i % 4];
    }
    fwrite($s, $head . $mask . $masked);
}

/** @param resource $s */
function ws_read_exact($s, int $n): string
{
    $out = '';
    while (strlen($out) < $n) {
        $chunk = fread($s, $n - strlen($out));
        if ($chunk === false || $chunk === '') {
            fail('connection ended while reading a frame');
        }
        $out .= $chunk;
    }
    return $out;
}

/**
 * One whole message: continuation frames are joined until FIN (RFC 6455 section 5.4; the daemon
 * fragments big outbound messages). Control frames between fragments are not expected here.
 *
 * @param resource $s
 * @return array{0:int, 1:string} [opcode of the first frame, payload]
 */
function ws_read($s): array
{
    $first = null;
    $payload = '';
    do {
        $h = ws_read_exact($s, 2);
        $fin = (ord($h[0]) & 0x80) !== 0;
        $opcode = ord($h[0]) & 0x0F;
        $length = ord($h[1]) & 0x7F;
        if ($length === 126) {
            $length = unpack('n', ws_read_exact($s, 2))[1];
        } elseif ($length === 127) {
            $length = unpack('J', ws_read_exact($s, 8))[1];
        }
        $payload .= ws_read_exact($s, $length);
        $first ??= $opcode;
        if ($first === 0 || ($opcode !== 0 && $opcode !== $first && $first <= 2)) {
            fail('unexpected frame sequence (first opcode ' . $first . ', then ' . $opcode . ')');
        }
    } while (!$fin);
    return [$first, $payload];
}
