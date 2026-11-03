<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Ipc\Codec;
use Wsrv\ProtocolError;

// Golden vectors: the bytes are written out by hand from docs/IPC_PROTOCOL.md section 2 and 3, not
// produced by the code under test, so an encoder that is wrong in a self-consistent way still fails.

test('codec: PING is length 2, type 6, flags 0', function () {
    assertSame(hex('00 00 00 02 06 00'), Codec::ping());
});

test('codec: HELLO carries version 0.1 and the token', function () {
    // length = 2 + (2+2+2+3) = 11
    assertSame(hex('00 00 00 0b 01 00  00 00  00 01  00 03  61 62 63'), Codec::hello('abc'));
    assertSame(hex('00 00 00 08 01 00  00 00  00 01  00 00'), Codec::hello());
});

test('codec: SEND_TEXT is request, connection, payload', function () {
    // length = 2 + 4 + 8 + 2 = 16
    assertSame(
        hex('00 00 00 10 03 00  00 00 00 01  00 00 00 00 00 00 00 02  68 69'),
        Codec::sendText(1, 2, 'hi')
    );
});

test('codec: SEND_BINARY keeps arbitrary bytes', function () {
    assertSame(
        hex('00 00 00 10 04 00  00 00 00 07  00 00 00 00 00 00 01 00  00 ff'),
        Codec::sendBinary(7, 256, "\x00\xff")
    );
});

test('codec: CLOSE is request, connection, code, reason', function () {
    // length = 2 + 4 + 8 + 2 + 3 = 19
    assertSame(
        hex('00 00 00 13 05 00  00 00 00 09  00 00 00 00 00 00 00 05  0f a1  62 79 65'),
        Codec::close(9, 5, 4001, 'bye')
    );
});

test('codec: ids of 0 are refused (0 is reserved)', function () {
    assertThrows(ProtocolError::class, fn () => Codec::sendText(0, 1, 'x'), 'request 0');
    assertThrows(ProtocolError::class, fn () => Codec::sendText(1, 0, 'x'), 'conn 0');
    assertThrows(ProtocolError::class, fn () => Codec::sendText(0x100000000, 1, 'x'), 'request > u32');
});

test('codec: the largest request id encodes', function () {
    assertSame(hex('ff ff ff ff'), substr(Codec::sendText(0xFFFFFFFF, 1, ''), 6, 4));
});

test('codec: a token or close reason that is too long is refused', function () {
    assertSame(true, strlen(Codec::hello(str_repeat('a', 256))) > 0);
    assertThrows(ProtocolError::class, fn () => Codec::hello(str_repeat('a', 257)));
    assertSame(true, strlen(Codec::close(1, 1, 1000, str_repeat('r', 123))) > 0);
    assertThrows(ProtocolError::class, fn () => Codec::close(1, 1, 1000, str_repeat('r', 124)));
});

test('codec: a close code must fit 16 bits', function () {
    assertThrows(ProtocolError::class, fn () => Codec::close(1, 1, 70000));
    assertThrows(ProtocolError::class, fn () => Codec::close(1, 1, -1));
});
