<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

use Wsrv\MessageTooLarge;
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
    public const VERSION_MINOR = 3;  // 0.3: the broker frames (ADR-0028); 0.2 was the handshake hook

    public const TYPE_HELLO = 1;
    public const TYPE_HELLO_OK = 2;
    public const TYPE_SEND_TEXT = 3;
    public const TYPE_SEND_BINARY = 4;
    public const TYPE_CLOSE = 5;
    public const TYPE_PING = 6;
    public const TYPE_PONG = 7;
    public const TYPE_EVENT = 8;
    public const TYPE_STATUS = 9;
    public const TYPE_HANDSHAKE_ACCEPT = 10;
    public const TYPE_HANDSHAKE_REJECT = 11;
    public const TYPE_SUBSCRIBE = 12;
    public const TYPE_UNSUBSCRIBE = 13;
    public const TYPE_PUBLISH = 14;

    public const HEADER_SIZE = 6;
    /** The topic is a u16 length in PUBLISH. The daemon's own (smaller) limit decides the rest. */
    public const MAX_TOPIC = 0xFFFF;
    public const MAX_TOKEN = 256;
    /** RFC 6455 section 5.5: a control frame carries at most 125 bytes, 2 of them the close code. */
    public const MAX_CLOSE_REASON = 123;
    /** The longest subprotocol name an answer may carry. */
    public const MAX_SUBPROTOCOL = 256;
    /** The HTTP statuses the daemon accepts for a refused upgrade (ADR-0018 section 3). */
    public const REJECT_STATUSES = [400, 401, 403, 404, 429, 500, 503];
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

    /** Let an upgrade in; $subprotocol is one the client offered, or '' for none. */
    public static function handshakeAccept(int $request, int $conn, string $subprotocol = ''): string
    {
        if (strlen($subprotocol) > self::MAX_SUBPROTOCOL) {
            throw new ProtocolError('subprotocol is longer than ' . self::MAX_SUBPROTOCOL . ' bytes');
        }
        return self::frame(self::TYPE_HANDSHAKE_ACCEPT, self::commandHead($request, $conn) . $subprotocol);
    }

    /** Refuse an upgrade with one of REJECT_STATUSES. */
    public static function handshakeReject(int $request, int $conn, int $status): string
    {
        if (!in_array($status, self::REJECT_STATUSES, true)) {
            throw new ProtocolError(
                'status ' . $status . ' is not one of ' . implode(', ', self::REJECT_STATUSES)
            );
        }
        return self::frame(self::TYPE_HANDSHAKE_REJECT, self::commandHead($request, $conn) . pack('n', $status));
    }

    /** Put connection $conn in $topic (a room). The application decides who may; the daemon never asks a browser. */
    public static function subscribe(int $request, int $conn, string $topic): string
    {
        return self::frame(self::TYPE_SUBSCRIBE, self::commandHead($request, $conn) . self::topic($topic));
    }

    public static function unsubscribe(int $request, int $conn, string $topic): string
    {
        return self::frame(self::TYPE_UNSUBSCRIBE, self::commandHead($request, $conn) . self::topic($topic));
    }

    /**
     * Send $payload to every subscriber of $topic except connection $except (0 = nobody). A text
     * message must be valid UTF-8 (the daemon answers INVALID_UTF8 otherwise).
     */
    public static function publish(int $request, string $topic, string $payload, bool $binary = false, int $except = 0): string
    {
        if ($request < 1 || $request > 0xFFFFFFFF) {
            throw new ProtocolError('request id must be 1..4294967295 (0 is reserved for the handshake)');
        }
        if ($except < 0) {
            throw new ProtocolError('except must be a connection id or 0');
        }
        return self::frame(
            self::TYPE_PUBLISH,
            pack('NCJn', $request, $binary ? 2 : 1, $except, strlen(self::topic($topic))) . $topic . $payload
        );
    }

    /** The characters the broker accepts in a topic (ADR-0024 section 2); anything else is refused here. */
    public static function topic(string $topic): string
    {
        if ($topic === '' || strlen($topic) > self::MAX_TOPIC || preg_match('/\A[A-Za-z0-9._:\/@-]+\z/', $topic) !== 1) {
            throw new ProtocolError('a topic is 1 or more of letters, digits and . _ : / @ -');
        }
        return $topic;
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
            throw new MessageTooLarge('frame is larger than the protocol ceiling');
        }
        return pack('NCC', $length, $type, 0) . $body;
    }
}
