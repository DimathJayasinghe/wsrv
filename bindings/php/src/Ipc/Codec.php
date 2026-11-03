<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

use Wsrv\ProtocolError;

/**
 * Encoders for the frames an application sends (docs/IPC_PROTOCOL.md). Every frame is
 *
 *     u32 length   big-endian; counts the bytes AFTER this field: type + flags + body
 *     u8  type
 *     u8  flags    reserved, always 0
 *     ... body
 *
 * Encoders refuse what no daemon could accept (a request id of 0, a connection id of 0, a token or
 * close reason that is too long): better a clear exception here than a closed link later.
 */
final class Codec
{
    public const VERSION_MAJOR = 0;
    public const VERSION_MINOR = 1;

    public const TYPE_HELLO = 1;
    public const TYPE_HELLO_OK = 2;
    public const TYPE_SEND_TEXT = 3;
    public const TYPE_SEND_BINARY = 4;
    public const TYPE_CLOSE = 5;
    public const TYPE_PING = 6;
    public const TYPE_PONG = 7;
    public const TYPE_EVENT = 8;
    public const TYPE_STATUS = 9;

    public const HEADER_SIZE = 6;
    public const MAX_TOKEN = 256;
    /** RFC 6455 section 5.5: a control frame carries at most 125 bytes, 2 of them the close code. */
    public const MAX_CLOSE_REASON = 123;
    /** No setting may raise the daemon's frame limit above this (64 MiB). */
    public const FRAME_CEILING = 64 * 1024 * 1024;

    public static function hello(string $token = ''): string
    {
        if (strlen($token) > self::MAX_TOKEN) {
            throw new ProtocolError('token is longer than ' . self::MAX_TOKEN . ' bytes');
        }
        return self::frame(
            self::TYPE_HELLO,
            pack('nnn', self::VERSION_MAJOR, self::VERSION_MINOR, strlen($token)) . $token
        );
    }

    public static function sendText(int $request, int $conn, string $text): string
    {
        return self::frame(self::TYPE_SEND_TEXT, self::commandHead($request, $conn) . $text);
    }

    public static function sendBinary(int $request, int $conn, string $data): string
    {
        return self::frame(self::TYPE_SEND_BINARY, self::commandHead($request, $conn) . $data);
    }

    public static function close(int $request, int $conn, int $code = 1000, string $reason = ''): string
    {
        if ($code < 0 || $code > 0xFFFF) {
            throw new ProtocolError('close code must fit 16 bits');
        }
        if (strlen($reason) > self::MAX_CLOSE_REASON) {
            throw new ProtocolError('close reason is longer than ' . self::MAX_CLOSE_REASON . ' bytes');
        }
        return self::frame(self::TYPE_CLOSE, self::commandHead($request, $conn) . pack('n', $code) . $reason);
    }

    public static function ping(): string
    {
        return self::frame(self::TYPE_PING, '');
    }

    /** u32 request id and u64 connection id, both nonzero. */
    private static function commandHead(int $request, int $conn): string
    {
        if ($request < 1 || $request > 0xFFFFFFFF) {
            throw new ProtocolError('request id must be 1..4294967295 (0 is reserved for the handshake)');
        }
        if ($conn < 1) {
            throw new ProtocolError('connection id must be positive');
        }
        return pack('NJ', $request, $conn);
    }

    private static function frame(int $type, string $body): string
    {
        $length = 2 + strlen($body);
        if ($length > self::FRAME_CEILING) {
            throw new ProtocolError('frame is larger than the protocol ceiling');
        }
        return pack('NCC', $length, $type, 0) . $body;
    }
}
