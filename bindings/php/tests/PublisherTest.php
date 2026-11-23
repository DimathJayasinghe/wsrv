<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\PublishFailed;
use Wsrv\Publisher;
use Wsrv\PublishResult;

// The HTTP bytes are written out by hand from docs/OPERATIONS.md 3.1.1. The transport is replaced
// by a closure that records the request and returns a canned answer; the real socket is exercised
// against a real wsrvd in interop.php.

const PUBLISH_TOKEN = 'a-publish-token-of-at-least-32-bytes!';

function answer(int $status, string $reason, string $body): string
{
    return "HTTP/1.1 $status $reason\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " . strlen($body)
        . "\r\nConnection: close\r\n\r\n" . $body;
}

/** @return array{Publisher, stdClass} */
function publisherAnswering(string $raw): array
{
    $seen = new stdClass();
    $seen->request = null;
    $publisher = new Publisher('http://wsrvd:9200', PUBLISH_TOKEN, 5.0, function (string $host, string $request, int $port, float $timeout) use ($raw, $seen): string {
        $seen->request = $request;
        $seen->host = $host;
        $seen->port = $port;
        return $raw;
    });
    return [$publisher, $seen];
}

test('publisher: the request is exactly what the endpoint expects', function () {
    [$publisher, $seen] = publisherAnswering(answer(200, 'OK', "subscribers=3 accepted=2 refused=1\n"));
    $result = $publisher->publish('room.7', 'hello', false, 42);
    assertSame(
        "POST /publish?topic=room.7&except=42 HTTP/1.1\r\nHost: wsrvd:9200\r\n"
        . 'Authorization: Bearer ' . PUBLISH_TOKEN . "\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
        $seen->request
    );
    assertSame('wsrvd', $seen->host);
    assertSame(9200, $seen->port);
    assertTrue($result instanceof PublishResult);
    assertSame([3, 2, 1], [$result->subscribers, $result->accepted, $result->refused]);
});

test('publisher: binary adds binary=1, no except adds nothing, the port defaults to 80', function () {
    $seen = new stdClass();
    $publisher = new Publisher('http://wsrvd', PUBLISH_TOKEN, 5.0, function (string $host, string $request, int $port) use ($seen): string {
        $seen->request = $request;
        $seen->port = $port;
        return answer(200, 'OK', "subscribers=0 accepted=0 refused=0\n");
    });
    $publisher->publish('t', "\xff\x00", true);
    assertSame(true, str_starts_with($seen->request, "POST /publish?topic=t&binary=1 HTTP/1.1\r\n"));
    assertSame(true, str_ends_with($seen->request, "\r\n\r\n\xff\x00"));
    assertSame(80, $seen->port);
});

test('publisher: every non-200 is a PublishFailed that says whether to retry', function () {
    foreach ([[401, false], [400, false], [413, false], [415, false], [429, true], [503, true]] as [$status, $retry]) {
        [$publisher] = publisherAnswering(answer($status, 'Whatever', "nope\n"));
        $e = assertThrows(PublishFailed::class, fn () => $publisher->publish('t', 'x'), (string) $status);
        assertSame($status, $e->status);
        assertSame("nope\n", $e->body);
        assertSame($retry, $e->isRetryable(), "retryable $status");
    }
});

test('publisher: a 429 still carries the counts for the caller to read', function () {
    [$publisher] = publisherAnswering(answer(429, 'Too Many Requests', "subscribers=2 accepted=0 refused=2\n"));
    $e = assertThrows(PublishFailed::class, fn () => $publisher->publish('t', 'x'));
    assertSame("subscribers=2 accepted=0 refused=2\n", $e->body);
});

test('publisher: an answer that is not HTTP, or a 200 without counts, is a failure with status 0 or 200', function () {
    [$garbage] = publisherAnswering('what is this');
    $e = assertThrows(PublishFailed::class, fn () => $garbage->publish('t', 'x'));
    assertSame(0, $e->status);
    assertSame(true, $e->isRetryable());
    [$odd] = publisherAnswering(answer(200, 'OK', "hello\n"));
    $e = assertThrows(PublishFailed::class, fn () => $odd->publish('t', 'x'));
    assertSame(200, $e->status);
});

test('publisher: bad input is refused before any request is made', function () {
    $called = false;
    $transport = function () use (&$called): string {
        $called = true;
        return '';
    };
    foreach (['https://wsrvd:9200', 'ftp://x', 'wsrvd:9200', 'http://', 'http://user@wsrvd', 'http://wsrvd/path', 'http://wsrvd?x=1'] as $url) {
        assertThrows(InvalidArgumentException::class, fn () => new Publisher($url, PUBLISH_TOKEN, 5.0, $transport), $url);
    }
    foreach (['', "tok\n", 'a b', "tab\there"] as $token) {
        assertThrows(InvalidArgumentException::class, fn () => new Publisher('http://wsrvd', $token, 5.0, $transport), show($token));
    }
    $publisher = new Publisher('http://wsrvd', PUBLISH_TOKEN, 5.0, $transport);
    foreach (['', 'a b', 'a%2Fb', "a\r\nHost: evil", 'room!'] as $topic) {
        assertThrows(\Wsrv\ProtocolError::class, fn () => $publisher->publish($topic, 'x'), show($topic));
    }
    assertThrows(InvalidArgumentException::class, fn () => $publisher->publish('t', 'x', false, -5));
    assertSame(false, $called, 'nothing was sent');
});

test('publisher: an unreachable endpoint is a retryable failure, not a warning', function () {
    $publisher = new Publisher('http://127.0.0.1:1', PUBLISH_TOKEN, 1.0);  // port 1: nothing listens
    $e = assertThrows(PublishFailed::class, fn () => $publisher->publish('t', 'x'));
    assertSame(0, $e->status);
    assertSame(true, $e->isRetryable());
});
