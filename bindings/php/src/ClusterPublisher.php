<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/**
 * Publish to a room on several wsrvd nodes (docs/DEPLOYMENT.md section 7). Rooms live inside each
 * daemon, so a message for a room must be sent to every daemon that may have a subscriber. There is
 * no backplane between daemons in 1.0; this class is the supported way to scale out.
 *
 *     $cluster = new ClusterPublisher(['http://10.0.0.5:9200', 'http://10.0.0.6:9200'], $token);
 *     $result = $cluster->publish('room.7', $json);
 *     if (!$result->complete()) { ... $result->failures names the nodes that did not take it ... }
 *
 * The nodes are asked one after another with the same message and the same token (use one
 * publish token for the whole cluster). A node that fails does not stop the others. If at least one
 * node answered you get a ClusterPublishResult, whose failures list says who did not; if none did,
 * PublishFailed is thrown, with isRetryable() true only if every node's failure was retryable.
 * Per-node timeouts add up in the worst case, so keep `$timeout` short (default 2 seconds).
 */
final class ClusterPublisher
{
    /** @var array<string, Publisher> keyed by node URL, in the order given */
    private array $nodes = [];

    /**
     * @param list<string> $baseUrls http://host[:port] of each node's publish endpoint
     * @param (callable(string, string, int, float): string)|null $transport for tests, as in Publisher
     */
    public function __construct(
        array $baseUrls,
        string $token,
        float $timeout = 2.0,
        mixed $transport = null,
    ) {
        if ($baseUrls === []) {
            throw new \InvalidArgumentException('a cluster needs at least one node URL');
        }
        foreach ($baseUrls as $url) {
            $normal = rtrim($url, '/');
            if (isset($this->nodes[$normal])) {
                throw new \InvalidArgumentException("the node $normal is listed twice: the message would be delivered twice");
            }
            $this->nodes[$normal] = new Publisher($normal, $token, $timeout, $transport);
        }
    }

    /**
     * @param string $topic   letters, digits and . _ : / @ - (the broker's rule)
     * @param int    $except  connection id to leave out on the node that owns it; 0 = nobody
     * @throws PublishFailed when no node took the message
     */
    public function publish(string $topic, string $payload, bool $binary = false, int $except = 0): ClusterPublishResult
    {
        Ipc\Codec::topic($topic);  // refuse a bad topic once, before any node is asked
        $subscribers = $accepted = $refused = 0;
        $failures = [];
        foreach ($this->nodes as $url => $node) {
            try {
                $result = $node->publish($topic, $payload, $binary, $except);
                $subscribers += $result->subscribers;
                $accepted += $result->accepted;
                $refused += $result->refused;
            } catch (PublishFailed $e) {
                $failures[$url] = $e;
            }
        }
        if (count($failures) === count($this->nodes)) {
            throw $this->allFailed($failures);
        }
        return new ClusterPublishResult(new PublishResult($subscribers, $accepted, $refused), $failures);
    }

    /** @param array<string, PublishFailed> $failures */
    private function allFailed(array $failures): PublishFailed
    {
        $retryable = true;
        $parts = [];
        foreach ($failures as $url => $failure) {
            $retryable = $retryable && $failure->isRetryable();
            $parts[] = "$url: " . $failure->getMessage();
        }
        // The caller only asks isRetryable(): 0 (no answer) is retryable, 400 stands for "will fail again".
        return new PublishFailed('no node took the message (' . implode('; ', $parts) . ')', $retryable ? 0 : 400);
    }
}
