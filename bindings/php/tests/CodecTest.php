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

test('codec: HELLO carries version 0.3 and the token', function () {
    // length = 2 + (2+2+2+3) = 11
    assertSame(hex('00 00 00 0b 01 00  00 00  00 03  00 03  61 62 63'), Codec::hello('abc'));
    assertSame(hex('00 00 00 08 01 00  00 00  00 03  00 00'), Codec::hello());
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

test('codec: HANDSHAKE_ACCEPT is request, connection, subprotocol', function () {
    // length = 2 + 4 + 8 + 4 = 18
    assertSame(
        hex('00 00 00 12 0a 00  00 00 00 01  00 00 00 00 00 00 00 05  63 68 61 74'),
        Codec::handshakeAccept(1, 5, 'chat')
    );
    assertSame(
        hex('00 00 00 0e 0a 00  00 00 00 02  00 00 00 00 00 00 00 05'),
        Codec::handshakeAccept(2, 5)
    );
});

test('codec: HANDSHAKE_REJECT is request, connection, status', function () {
    // length = 2 + 4 + 8 + 2 = 16; 403 = 0x0193
    assertSame(
        hex('00 00 00 10 0b 00  00 00 00 09  00 00 00 00 00 00 00 05  01 93'),
        Codec::handshakeReject(9, 5, 403)
    );
});

test('codec: handshake answers refuse what no daemon would accept', function () {
    assertThrows(ProtocolError::class, fn () => Codec::handshakeAccept(0, 5), 'request 0');
    assertThrows(ProtocolError::class, fn () => Codec::handshakeAccept(1, 0), 'connection 0');
    assertThrows(ProtocolError::class, fn () => Codec::handshakeAccept(1, 5, str_repeat('p', 257)));
    assertTrue(strlen(Codec::handshakeAccept(1, 5, str_repeat('p', 256))) > 0, 'exactly 256 is fine');
    assertThrows(ProtocolError::class, fn () => Codec::handshakeReject(0, 5, 403), 'request 0');
    assertThrows(ProtocolError::class, fn () => Codec::handshakeReject(1, 0, 403), 'connection 0');
    foreach ([0, 200, 302, 418, 502, 599, 65536] as $status) {
        assertThrows(ProtocolError::class, fn () => Codec::handshakeReject(1, 5, $status), "status $status");
    }
    foreach ([400, 401, 403, 404, 429, 500, 503] as $status) {
        assertTrue(strlen(Codec::handshakeReject(1, 5, $status)) === 20, "status $status");
    }
});


// IPC 0.3 (ADR-0028), written out by hand from docs/IPC_PROTOCOL.md.
test('codec: SUBSCRIBE and UNSUBSCRIBE are request, connection, topic to the end', function () {
    // length = 2 + 4 + 8 + 4 = 18
    assertSame(
        hex('00 00 00 12 0c 00  00 00 00 01  00 00 00 00 00 00 00 02  72 6f 6f 6d'),
        Codec::subscribe(1, 2, 'room')
    );
    assertSame(
        hex('00 00 00 12 0d 00  00 00 00 01  00 00 00 00 00 00 00 02  72 6f 6f 6d'),
        Codec::unsubscribe(1, 2, 'room')
    );
});

test('codec: PUBLISH is request, kind, except, topic length, topic, payload', function () {
    // length = 2 + 4 + 1 + 8 + 2 + 1 + 2 = 20
    assertSame(
        hex('00 00 00 14 0e 00  00 00 00 05  01  00 00 00 00 00 00 00 00  00 01  74  68 69'),
        Codec::publish(5, 't', 'hi')
    );
    // binary, leaving connection 258 out
    assertSame(
        hex('00 00 00 14 0e 00  00 00 00 05  02  00 00 00 00 00 00 01 02  00 01  74  ff 00'),
        Codec::publish(5, 't', "\xff\x00", true, 258)
    );
});

test('codec: a topic outside the broker alphabet, or an id of 0, is refused before it is sent', function () {
    foreach (['', 'a b', "a\nb", 'a%20b', 'room!', 'a?b', str_repeat('a', 65536)] as $bad) {
        assertThrows(ProtocolError::class, fn () => Codec::subscribe(1, 2, $bad), 'subscribe ' . show($bad));
        assertThrows(ProtocolError::class, fn () => Codec::publish(1, $bad, 'x'), 'publish ' . show($bad));
    }
    foreach (['a', 'room.1', 'a_b:c/d@e-f', 'ROOM9'] as $good) {
        assertSame(true, strlen(Codec::subscribe(1, 2, $good)) > 0, $good);
    }
    assertThrows(ProtocolError::class, fn () => Codec::subscribe(0, 2, 'r'), 'request 0');
    assertThrows(ProtocolError::class, fn () => Codec::subscribe(1, 0, 'r'), 'conn 0');
    assertThrows(ProtocolError::class, fn () => Codec::publish(0, 'r', 'x'), 'request 0');
    assertThrows(ProtocolError::class, fn () => Codec::publish(1, 'r', 'x', false, -1), 'negative except');
});
