<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Ipc\Event;
use Wsrv\Ipc\EventKind;
use Wsrv\Ipc\FrameDecoder;
use Wsrv\Ipc\HelloOk;
use Wsrv\Ipc\Pong;
use Wsrv\Ipc\StatusReply;
use Wsrv\ProtocolError;

/** An EVENT frame built by hand. */
function event_frame(int $kind, int $conn, int $code, int $clean, string $data): string
{
    $body = pack('CJnC', $kind, $conn, $code, $clean) . $data;
    return pack('NCC', 2 + strlen($body), 8, 0) . $body;
}

test('decoder: HELLO_OK', function () {
    $d = new FrameDecoder(1024);
    $d->feed(hex('00 00 00 0a 02 00  00 00  00 01  00 10 00 00'));
    $m = $d->next();
    assertTrue($m instanceof HelloOk);
    assertSame([0, 1, 1048576], [$m->major, $m->minor, $m->maxFrame]);
    assertSame(null, $d->next());
});

test('decoder: STATUS and PONG', function () {
    $d = new FrameDecoder(1024);
    $d->feed(hex('00 00 00 07 09 00  00 00 00 2a  05') . hex('00 00 00 02 07 00'));
    $s = $d->next();
    assertTrue($s instanceof StatusReply);
    assertSame([42, 5], [$s->request, $s->status]);
    assertTrue($d->next() instanceof Pong);
});

test('decoder: every event kind', function () {
    $d = new FrameDecoder(1024);
    $d->feed(event_frame(1, 7, 0, 0, '10.0.0.1:5000'));
    $d->feed(event_frame(2, 7, 0, 0, 'héllo'));
    $d->feed(event_frame(3, 7, 0, 0, "\x00\xff"));
    $d->feed(event_frame(4, 7, 1005, 0, 'why'));
    $d->feed(event_frame(5, 7, 0, 0, 'tls'));
    $kinds = [];
    while (($m = $d->next()) !== null) {
        assertTrue($m instanceof Event);
        $kinds[] = $m->kind;
    }
    assertSame([EventKind::Open, EventKind::Text, EventKind::Binary, EventKind::Close, EventKind::Error], $kinds);
});

test('decoder: a Close event keeps code, clean flag and reason', function () {
    $d = new FrameDecoder(1024);
    $d->feed(event_frame(4, 3, 1006, 0, ''));
    $d->feed(event_frame(4, 4, 1000, 1, 'bye'));
    $a = $d->next();
    $b = $d->next();
    assertTrue($a instanceof Event && $b instanceof Event);
    assertSame([3, 1006, false, ''], [$a->conn, $a->closeCode, $a->clean, $a->data]);
    assertSame([4, 1000, true, 'bye'], [$b->conn, $b->closeCode, $b->clean, $b->data]);
});

test('decoder: frames arriving one byte at a time', function () {
    $bytes = event_frame(2, 9, 0, 0, 'split me') . hex('00 00 00 02 07 00');
    $d = new FrameDecoder(1024);
    $got = [];
    foreach (str_split($bytes) as $i => $byte) {
        $d->feed($byte);
        while (($m = $d->next()) !== null) {
            $got[] = $m;
        }
    }
    assertSame(2, count($got));
    assertTrue($got[0] instanceof Event && $got[0]->data === 'split me');
    assertTrue($got[1] instanceof Pong);
});

test('decoder: many frames in one feed', function () {
    $d = new FrameDecoder(1 << 20);
    $d->feed(str_repeat(event_frame(2, 1, 0, 0, 'x'), 1000));
    $n = 0;
    while ($d->next() !== null) {
        $n++;
    }
    assertSame(1000, $n);
    assertSame(0, $d->pending());
});

test('decoder: an oversized declared length fails before the body arrives', function () {
    $d = new FrameDecoder(1024);
    $d->feed(pack('NCC', 1025, 8, 0));  // header only; the 1 KiB body never comes
    assertThrows(ProtocolError::class, fn () => $d->next());
});

test('decoder: a frame exactly at the limit is accepted', function () {
    $d = new FrameDecoder(2 + 12 + 5);
    $d->feed(event_frame(2, 1, 0, 0, 'abcde'));
    assertTrue($d->next() instanceof Event);
});

test('decoder: malformed headers are errors', function () {
    foreach (
        [
            'length 0' => hex('00 00 00 00 07 00'),
            'length 1' => hex('00 00 00 01 07 00'),
            'flags' => hex('00 00 00 02 07 01'),
            'unknown type' => hex('00 00 00 02 c8 00'),
            'type 0' => hex('00 00 00 02 00 00'),
        ] as $what => $bytes
    ) {
        $d = new FrameDecoder(1024);
        $d->feed($bytes);
        assertThrows(ProtocolError::class, fn () => $d->next(), $what);
    }
});

test('decoder: frame types only an application may send are errors from the daemon', function () {
    foreach ([1, 3, 4, 5, 6] as $type) {
        $d = new FrameDecoder(1024);
        $d->feed(pack('NCC', 2, $type, 0));
        assertThrows(ProtocolError::class, fn () => $d->next(), "type $type");
    }
});

test('decoder: wrong body sizes are errors', function () {
    foreach (
        [
            'HELLO_OK short' => hex('00 00 00 09 02 00 00 00 00 01 00 10 00'),
            'HELLO_OK long' => hex('00 00 00 0b 02 00 00 00 00 01 00 10 00 00 00'),
            'STATUS short' => hex('00 00 00 06 09 00 00 00 00 01'),
            'STATUS long' => hex('00 00 00 08 09 00 00 00 00 01 00 00'),
            'PONG with body' => hex('00 00 00 03 07 00 ff'),
            'EVENT short' => hex('00 00 00 0d 08 00 02 00 00 00 00 00 00 00 01 00 00'),
        ] as $what => $bytes
    ) {
        $d = new FrameDecoder(1024);
        $d->feed($bytes);
        assertThrows(ProtocolError::class, fn () => $d->next(), $what);
    }
});

test('decoder: inconsistent events are errors', function () {
    foreach (
        [
            'kind 0' => event_frame(0, 1, 0, 0, ''),
            'kind 6' => event_frame(6, 1, 0, 0, ''),
            'conn 0' => event_frame(2, 0, 0, 0, ''),
            'conn >= 2^63' => event_frame(2, PHP_INT_MIN, 0, 0, ''),
            'close code on Text' => event_frame(2, 1, 1000, 0, ''),
            'clean on Open' => event_frame(1, 1, 0, 1, ''),
            'clean = 2' => event_frame(4, 1, 1000, 2, ''),
        ] as $what => $bytes
    ) {
        $d = new FrameDecoder(1024);
        $d->feed($bytes);
        assertThrows(ProtocolError::class, fn () => $d->next(), $what);
    }
});

test('decoder: after an error it stays failed', function () {
    $d = new FrameDecoder(1024);
    $d->feed(hex('00 00 00 02 c8 00'));
    assertThrows(ProtocolError::class, fn () => $d->next());
    $d->feed(hex('00 00 00 02 07 00'));  // a perfectly good PONG after the garbage
    assertThrows(ProtocolError::class, fn () => $d->next(), 'must not resynchronise');
});

test('decoder: consumed bytes are released', function () {
    $d = new FrameDecoder(1 << 20);
    for ($i = 0; $i < 200; $i++) {
        $d->feed(event_frame(2, 1, 0, 0, str_repeat('x', 1000)));
        $d->next();
    }
    assertSame(0, $d->pending());
});
