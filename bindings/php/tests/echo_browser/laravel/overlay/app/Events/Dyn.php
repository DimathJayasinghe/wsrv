<?php

namespace App\Events;

use Illuminate\Broadcasting\Channel;
use Illuminate\Broadcasting\EncryptedPrivateChannel;
use Illuminate\Broadcasting\InteractsWithSockets;
use Illuminate\Broadcasting\PresenceChannel;
use Illuminate\Broadcasting\PrivateChannel;
use Illuminate\Contracts\Broadcasting\ShouldBroadcastNow;

/** An event whose channel, name and payload the test page chooses. */
class Dyn implements ShouldBroadcastNow
{
    use InteractsWithSockets;

    public function __construct(private string $channelName, private string $eventName, private array $data)
    {
    }

    public function broadcastOn(): array
    {
        $n = $this->channelName;
        return [match (true) {
            str_starts_with($n, 'private-encrypted-') => new EncryptedPrivateChannel(substr($n, 18)),
            str_starts_with($n, 'private-') => new PrivateChannel(substr($n, 8)),
            str_starts_with($n, 'presence-') => new PresenceChannel(substr($n, 9)),
            default => new Channel($n),
        }];
    }

    public function broadcastAs(): string
    {
        return $this->eventName;
    }

    public function broadcastWith(): array
    {
        return $this->data;
    }
}
