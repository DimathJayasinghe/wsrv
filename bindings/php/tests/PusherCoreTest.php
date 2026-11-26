<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\Pusher\BadFrame;
use Wsrv\Pusher\ChannelKind;
use Wsrv\Pusher\ChannelName;
use Wsrv\Pusher\Frame;
use Wsrv\Pusher\PresenceTable;
use Wsrv\Pusher\Signature;
use Wsrv\Pusher\SocketId;

// ---- Signature: the vectors are the examples in Pusher's own documentation ------------------------

const PUSHER_KEY = '278d425bdf160c739803';
const PUSHER_SECRET = '7ad3773142a6692b25b8';

test('pusher signature: a private channel matches the published vector', function () {
    assertSame(
        PUSHER_KEY . ':58df8b0c36d6982b82c3ecf6b4662e34fe8c25bba48f5369f135bf843651c3a4',
        Signature::authorize(PUSHER_KEY, PUSHER_SECRET, '1234.1234', 'private-foobar'),
    );
});

test('pusher signature: a presence channel signs the channel_data string too', function () {
    $data = '{"user_id":10,"user_info":{"name":"Mr. Pusher"}}';
    assertSame(
        PUSHER_KEY . ':afaed3695da2ffd16931f457e338e6c9f2921fa133ce7dac49f529792be6304c',
        Signature::authorize(PUSHER_KEY, PUSHER_SECRET, '1234.1234', 'presence-foobar', $data),
    );
});

test('pusher signature: verify accepts the right value and refuses every change', function () {
    $good = Signature::authorize(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-a');
    assertTrue(Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-a', null, $good));
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.3', 'private-a', null, $good), 'another socket');
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-b', null, $good), 'another channel');
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-a', '{}', $good), 'extra channel_data');
    assertTrue(!Signature::verify(PUSHER_KEY, 'other-secret-other-secret', '1.2', 'private-a', null, $good), 'another secret');
    assertTrue(!Signature::verify('otherkey', PUSHER_SECRET, '1.2', 'private-a', null, $good), 'another key');
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-a', null, ''), 'empty');
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-a', null, substr($good, 0, -1)), 'truncated');
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'private-a', null, strtoupper($good)), 'case');
});

test('pusher signature: a channel_data that was not signed does not verify', function () {
    $auth = Signature::authorize(PUSHER_KEY, PUSHER_SECRET, '1.2', 'presence-a', '{"user_id":"1"}');
    assertTrue(Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'presence-a', '{"user_id":"1"}', $auth));
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'presence-a', '{"user_id":"2"}', $auth), 'swapped user');
    assertTrue(!Signature::verify(PUSHER_KEY, PUSHER_SECRET, '1.2', 'presence-a', null, $auth), 'dropped');
});

// ---- SocketId -------------------------------------------------------------------------------------

test('pusher socket id: two numbers joined by a dot, and the connection is read back from it', function () {
    $id = SocketId::generate(4242);
    assertTrue(preg_match('/\A4242\.[1-9][0-9]{0,9}\z/', $id) === 1, 'shape: ' . $id);
    assertSame(4242, SocketId::connection($id));
    assertTrue(SocketId::generate(7) !== SocketId::generate(7), 'a random half');
});

test('pusher socket id: anything that is not digits.digits is refused', function () {
    foreach (['', '1', '1.', '.1', '1.2.3', 'a.1', '1.b', '-1.2', '1 .2', "1.2\n", '0.5', '99999999999999999999.1', '1e3.2'] as $bad) {
        assertSame(null, SocketId::connection($bad), 'bad: ' . json_encode($bad));
    }
    assertSame(9223372036854775807, SocketId::connection('9223372036854775807.1'));
});

// ---- ChannelName ----------------------------------------------------------------------------------

test('pusher channel name: kinds by prefix', function () {
    assertSame(ChannelKind::Public, ChannelName::kind('orders'));
    assertSame(ChannelKind::Public, ChannelName::kind('App.Models.User.1'));
    assertSame(ChannelKind::Private, ChannelName::kind('private-orders.7'));
    assertSame(ChannelKind::Private, ChannelName::kind('private-encrypted-orders.7'));
    assertSame(ChannelKind::Presence, ChannelName::kind('presence-room.1'));
    // A prefix needs its dash: these are public names that merely start with the word.
    assertSame(ChannelKind::Public, ChannelName::kind('private'));
    assertSame(ChannelKind::Public, ChannelName::kind('presenceroom'));
});

test('pusher channel name: only what wsrv can carry is valid, and cache channels are refused', function () {
    foreach (['a', 'private-x', 'presence-a.b:c/d@e-f_g', str_repeat('a', 128)] as $ok) {
        assertSame(null, ChannelName::problem($ok), 'ok: ' . $ok);
    }
    foreach (['', str_repeat('a', 129), 'a b', 'a=b', 'a,b', 'a;b', 'a%41', 'é', "a\n", 'a*', '#server-to-user-1'] as $bad) {
        assertTrue(ChannelName::problem($bad) !== null, 'bad: ' . json_encode($bad));
    }
    foreach (['cache-a', 'private-cache-a', 'presence-cache-a'] as $cache) {
        assertTrue(ChannelName::problem($cache) !== null, 'cache channel: ' . $cache);
    }
});

// ---- Frame ----------------------------------------------------------------------------------------

test('pusher frame: a client frame is decoded with its data left as the client sent it', function () {
    $f = Frame::decode('{"event":"pusher:subscribe","data":{"channel":"private-a","auth":"k:s"}}', 1024);
    assertSame('pusher:subscribe', $f->event);
    assertSame(null, $f->channel);
    assertSame(['channel' => 'private-a', 'auth' => 'k:s'], $f->object());
});

test('pusher frame: data may also be a JSON string, as Echo sends it', function () {
    $f = Frame::decode('{"event":"pusher:subscribe","data":"{\"channel\":\"a\"}"}', 1024);
    assertSame(['channel' => 'a'], $f->object());
    $g = Frame::decode('{"event":"client-x","channel":"private-a","data":{"n":1}}', 1024);
    assertSame('private-a', $g->channel);
    assertSame(['n' => 1], $g->object());
    $h = Frame::decode('{"event":"pusher:ping"}', 1024);
    assertSame(null, $h->object());
});

test('pusher frame: a string that is not an object is not an object', function () {
    assertSame(null, Frame::decode('{"event":"e","data":"hello"}', 1024)->object());
    assertSame(null, Frame::decode('{"event":"e","data":"123"}', 1024)->object());
    assertSame(null, Frame::decode('{"event":"e","data":5}', 1024)->object());
});

test('pusher frame: every malformed frame is a BadFrame and never anything else', function () {
    $bad = [
        '', 'not json', '[]', '5', '"x"', 'null', '{}', '{"event":5}', '{"event":""}', '{"event":null}',
        '{"event":"e","channel":5}', '{"event":"e","channel":["a"]}',
        '{"event":"' . str_repeat('e', 201) . '"}',
        '{"event":"e","data":' . str_repeat('[', 20) . str_repeat(']', 20) . '}',
        "{\"event\":\"e\"}\x00", '{"event":"e"} trailing', "\xff\xfe",
        '{"event":"e","data":"' . "\xff" . '"}',
    ];
    foreach ($bad as $text) {
        assertThrows(BadFrame::class, fn () => Frame::decode($text, 1024), show($text));
    }
});

test('pusher frame: a frame over the bound is refused before it is parsed', function () {
    $text = '{"event":"e","data":"' . str_repeat('a', 100) . '"}';
    Frame::decode($text, strlen($text));
    assertThrows(BadFrame::class, fn () => Frame::decode($text, strlen($text) - 1));
});

test('pusher frame: server frames carry data as a JSON string, as pusher-js expects', function () {
    assertSame(
        '{"event":"pusher:connection_established","data":"{\"socket_id\":\"1.2\",\"activity_timeout\":30}"}',
        Frame::encode('pusher:connection_established', ['socket_id' => '1.2', 'activity_timeout' => 30]),
    );
    assertSame('{"event":"pusher:pong"}', Frame::encode('pusher:pong'));
    assertSame(
        '{"event":"pusher_internal:subscription_succeeded","data":"{}","channel":"room"}',
        Frame::encode('pusher_internal:subscription_succeeded', new stdClass(), 'room'),
    );
});

test('pusher frame: encoding keeps slashes and UTF-8 readable and always yields valid UTF-8', function () {
    $text = Frame::encode('e', ['name' => "caf\u{e9}/x"], 'c');
    assertSame('{"event":"e","data":"{\"name\":\"caf' . "\u{e9}" . '/x\"}","channel":"c"}', $text);
    assertTrue(preg_match('//u', $text) === 1);
});

// ---- PresenceTable --------------------------------------------------------------------------------

test('pusher presence: a user is one member however many connections they have', function () {
    $p = new PresenceTable(3);
    assertSame(PresenceTable::ADDED, $p->add('presence-a', 10, '1', ['name' => 'A']));
    assertSame(PresenceTable::SAME_USER, $p->add('presence-a', 11, '1', ['name' => 'A']));
    assertSame(1, $p->count('presence-a'));
    assertSame(PresenceTable::STAYS, $p->remove('presence-a', 10));
    assertSame(1, $p->count('presence-a'));
    assertSame('1', $p->remove('presence-a', 11));
    assertSame(0, $p->count('presence-a'));
});

test('pusher presence: the summary lists ids, the hash and the count', function () {
    $p = new PresenceTable(10);
    $p->add('presence-a', 1, '7', ['name' => 'A']);
    $p->add('presence-a', 2, '8', null);
    assertSame(['count' => 2, 'ids' => ['7', '8'], 'hash' => ['7' => ['name' => 'A'], '8' => []]], $p->summary('presence-a'));
    assertSame(['count' => 0, 'ids' => [], 'hash' => []], $p->summary('presence-none'));
});

test('pusher presence: the member cap refuses a new user, never a second connection of a member', function () {
    $p = new PresenceTable(2);
    $p->add('presence-a', 1, 'a', null);
    $p->add('presence-a', 2, 'b', null);
    assertSame(PresenceTable::FULL, $p->add('presence-a', 3, 'c', null));
    assertSame(PresenceTable::SAME_USER, $p->add('presence-a', 4, 'a', null));
    assertSame(2, $p->count('presence-a'));
});

test('pusher presence: removing what is not there is harmless, and empty channels are forgotten', function () {
    $p = new PresenceTable(5);
    assertSame(PresenceTable::ABSENT, $p->remove('presence-none', 1));
    $p->add('presence-a', 1, 'x', null);
    assertSame(PresenceTable::ABSENT, $p->remove('presence-a', 99));
    $p->remove('presence-a', 1);
    assertSame(0, $p->channels());
});

test('pusher presence: user ids that look alike stay different users', function () {
    $p = new PresenceTable(10);
    $p->add('presence-a', 1, '1', null);
    assertSame(PresenceTable::ADDED, $p->add('presence-a', 2, '01', null));
    assertSame(PresenceTable::ADDED, $p->add('presence-a', 3, '1.0', null));
    assertSame(3, $p->count('presence-a'));
});

test('pusher signature: the comparison is the constant-time one (a timing difference cannot be tested, so the source is pinned)', function () {
    $source = (string) file_get_contents(__DIR__ . '/../src/Pusher/Signature.php');
    assertTrue(str_contains($source, 'hash_equals('), 'Signature::verify must compare with hash_equals');
    assertTrue(!str_contains($source, '=== $auth') && !str_contains($source, '== $auth'), 'no plain comparison of the signature');
});
