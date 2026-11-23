<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

use Wsrv\ClusterPublisher;
use Wsrv\PublishFailed;

// Publishing to every daemon (docs/DEPLOYMENT.md section 7): rooms live inside each wsrvd, so a
// message for a room must reach every node. Transports are canned, as in PublisherTest.

/** @param array<string, string|Throwable> $byHost raw answer (or exception) per "host:port" */
function cluster(array $byHost, array &$requests): ClusterPublisher
{
    return new ClusterPublisher(
        ['http://a:9200', 'http://b:9200', 'http://c:9201'],
        PUBLISH_TOKEN,
        1.0,
        function (string $host, string $request, int $port, float $timeout) use ($byHost, &$requests): string {
            $key = $host . ':' . $port;
            $requests[$key] = $request;
            $answer = $byHost[$key] ?? throw new LogicException("no canned answer for $key");
            if ($answer instanceof Throwable) {
                throw $answer;
            }
            return $answer;
        },
    );
}

test('cluster publisher: the same message goes to every node and the counts add up', function () {
    $requests = [];
    $cluster = cluster([
        'a:9200' => answer(200, 'OK', "subscribers=3 accepted=3 refused=0\n"),
        'b:9200' => answer(200, 'OK', "subscribers=2 accepted=1 refused=1\n"),
        'c:9201' => answer(200, 'OK', "subscribers=0 accepted=0 refused=0\n"),
    ], $requests);
    $result = $cluster->publish('room.7', 'hello', false, 42);
    assertSame(3, count($requests), 'every node was asked');
    foreach ($requests as $key => $request) {
        assertTrue(str_contains($request, "POST /publish?topic=room.7&except=42 HTTP/1.1"), "request to $key");
        assertTrue(str_ends_with($request, "\r\n\r\nhello"), "body to $key");
    }
    assertSame(5, $result->total->subscribers);
    assertSame(4, $result->total->accepted);
    assertSame(1, $result->total->refused);
    assertTrue($result->complete(), 'no node failed');
    assertSame([], $result->failures);
});

test('cluster publisher: a node that fails is reported, the others still get the message', function () {
    $requests = [];
    $cluster = cluster([
        'a:9200' => answer(200, 'OK', "subscribers=3 accepted=3 refused=0\n"),
        'b:9200' => new PublishFailed('cannot reach the publish endpoint: refused'),
        'c:9201' => answer(503, 'Service Unavailable', "shutting down\n"),
    ], $requests);
    $result = $cluster->publish('room.7', 'hello');
    assertSame(3, $result->total->subscribers, 'only the node that answered is counted');
    assertTrue(!$result->complete());
    assertSame(['http://b:9200', 'http://c:9201'], array_keys($result->failures));
    assertTrue($result->failures['http://b:9200']->isRetryable());
    assertSame(503, $result->failures['http://c:9201']->status);
});

test('cluster publisher: when no node took the message it throws, retryable only if every failure is', function () {
    $requests = [];
    $down = cluster([
        'a:9200' => new PublishFailed('cannot reach the publish endpoint: refused'),
        'b:9200' => new PublishFailed('cannot reach the publish endpoint: refused'),
        'c:9201' => answer(429, 'Too Many Requests', "busy\n"),
    ], $requests);
    $e = assertThrows(PublishFailed::class, fn () => $down->publish('room.7', 'x'));
    assertTrue($e->isRetryable(), 'unreachable and 429 are both "try later"');
    assertTrue(str_contains($e->getMessage(), 'http://a:9200'), 'the message names the nodes');

    $bad = cluster([
        'a:9200' => answer(401, 'Unauthorized', "bad token\n"),
        'b:9200' => new PublishFailed('cannot reach the publish endpoint: refused'),
        'c:9201' => answer(401, 'Unauthorized', "bad token\n"),
    ], $requests);
    $e2 = assertThrows(PublishFailed::class, fn () => $bad->publish('room.7', 'x'));
    assertTrue(!$e2->isRetryable(), 'a wrong token will not heal by waiting');
});

test('cluster publisher: bad input is refused before any request is made', function () {
    $requests = [];
    $cluster = cluster([], $requests);
    assertThrows(\Wsrv\ProtocolError::class, fn () => $cluster->publish('bad topic!', 'x'));
    assertSame([], $requests);
    assertThrows(InvalidArgumentException::class, fn () => new ClusterPublisher([], PUBLISH_TOKEN));
    assertThrows(InvalidArgumentException::class, fn () => new ClusterPublisher(['https://a:9200'], PUBLISH_TOKEN));
    assertThrows(InvalidArgumentException::class, fn () => new ClusterPublisher(['http://a:9200', 'http://a:9200'], PUBLISH_TOKEN), 'the same node twice would deliver twice');
});

test('cluster publisher: nodes are tried in the order given, one request each', function () {
    $order = [];
    $cluster = new ClusterPublisher(
        ['http://n1:1', 'http://n2:2', 'http://n3:3'],
        PUBLISH_TOKEN,
        1.0,
        function (string $host, string $request, int $port, float $timeout) use (&$order): string {
            $order[] = $host;
            return answer(200, 'OK', "subscribers=1 accepted=1 refused=0\n");
        },
    );
    $cluster->publish('t', 'x');
    assertSame(['n1', 'n2', 'n3'], $order);
});
