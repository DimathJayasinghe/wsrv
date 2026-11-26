<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

/** What a ClusterPublisher publish did: the counts summed over the nodes that answered, and who did not. */
final class ClusterPublishResult
{
    /**
     * @param PublishResult                $total    the sum over the nodes that answered 200
     * @param array<string, PublishFailed> $failures the nodes that did not, keyed by their URL
     */
    public function __construct(
        public readonly PublishResult $total,
        public readonly array $failures = [],
    ) {
    }

    /** True when every node took the message. */
    public function complete(): bool
    {
        return $this->failures === [];
    }
}
