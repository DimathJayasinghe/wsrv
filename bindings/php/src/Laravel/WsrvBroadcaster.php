<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Laravel;

use Illuminate\Broadcasting\BroadcastException;
use Illuminate\Broadcasting\Broadcasters\Broadcaster;
use Illuminate\Broadcasting\Broadcasters\UsePusherChannelConventions;
use Symfony\Component\HttpKernel\Exception\AccessDeniedHttpException;
use Wsrv\Publisher;
use Wsrv\PublishFailed;
use Wsrv\Pusher\ChannelKind;
use Wsrv\Pusher\ChannelName;
use Wsrv\Pusher\EncryptedChannels;
use Wsrv\Pusher\Frame;
use Wsrv\Pusher\GatewayConfig;
use Wsrv\Pusher\SocketId;

/**
 * Laravel's `broadcast()`, `Broadcast::channel()` and the `/broadcasting/auth` route, on wsrvd (ADR-0042).
 *
 * Laravel authorizes a private or presence channel exactly as with Pusher: this class gives the
 * browser a signature for its socket id (the gateway in the worker verifies it), and publishes
 * events to the daemon's HTTP publish endpoint, which delivers them to the browsers in the room.
 * The browser side is unchanged: Laravel Echo with `broadcaster: 'reverb'` or `'pusher'` pointed at
 * the daemon.
 *
 * Differences from Pusher that matter: channel names are limited to the daemon's topic alphabet
 * (letters, digits and `. _ : / @ -`, 128 bytes), and `toOthers()` works through the socket id of
 * the sender, which is the daemon's connection number.
 */
final class WsrvBroadcaster extends Broadcaster
{
    use UsePusherChannelConventions;

    public function __construct(
        private readonly GatewayConfig $config,
        private readonly Publisher $publisher,
        private readonly ?EncryptedChannels $encryption = null,
    ) {
    }

    /**
     * @param \Illuminate\Http\Request $request
     * @throws AccessDeniedHttpException
     */
    public function auth($request)
    {
        $channel = $request->channel_name;
        $socketId = $request->socket_id;
        if (!is_string($channel) || !is_string($socketId) || SocketId::connection($socketId) === null
            || ChannelName::problem($channel) !== null) {
            throw new AccessDeniedHttpException();
        }
        $normalized = $this->normalizeChannelName($channel);
        if ($this->isGuardedChannel($channel) && !$this->retrieveUser($request, $normalized)) {
            throw new AccessDeniedHttpException();
        }
        return parent::verifyUserCanAccessChannel($request, $normalized);
    }

    /**
     * @param \Illuminate\Http\Request $request
     * @param mixed $result what the channel callback returned: for a presence channel, the member's info
     * @return array<string, string>
     */
    public function validAuthenticationResponse($request, $result)
    {
        $channel = (string) $request->channel_name;
        $socketId = (string) $request->socket_id;
        $kind = ChannelName::kind($channel);

        if ($kind === ChannelKind::Public) {
            return [];   // public channels are never signed
        }
        $response = [];
        if ($kind === ChannelKind::Private) {
            $response['auth'] = $this->config->authorize($socketId, $channel);
        } else {
            $user = $this->retrieveUser($request, $this->normalizeChannelName($channel));
            if ($user === null) {
                throw new AccessDeniedHttpException();
            }
            $id = method_exists($user, 'getAuthIdentifierForBroadcasting')
                ? $user->getAuthIdentifierForBroadcasting()
                : $user->getAuthIdentifier();
            $member = ['user_id' => (string) $id];
            $info = self::memberInfo($result);
            if ($info !== null) {
                $member['user_info'] = $info;
            }
            $channelData = json_encode($member, JSON_THROW_ON_ERROR | JSON_UNESCAPED_SLASHES);
            if (strlen($channelData) > $this->config->maxChannelDataBytes) {
                throw new AccessDeniedHttpException(
                    'The presence data of this member is larger than ' . $this->config->maxChannelDataBytes . ' bytes'
                );
            }
            $response['auth'] = $this->config->authorize($socketId, $channel, $channelData);
            $response['channel_data'] = $channelData;
        }
        if (EncryptedChannels::isEncrypted($channel)) {
            if ($this->encryption === null) {
                throw new AccessDeniedHttpException('encrypted channels need encryption_master_key_base64 in the wsrv connection');
            }
            $response['shared_secret'] = $this->encryption->sharedSecretBase64($channel);
        }
        return $response;
    }

    /**
     * @param array<int, mixed> $channels
     * @param string $event
     * @param array<string, mixed> $payload
     * @throws BroadcastException
     */
    public function broadcast(array $channels, $event, array $payload = [])
    {
        $socket = $payload['socket'] ?? null;
        unset($payload['socket']);
        // The sender's own socket is left out (toOthers()). A socket id this daemon did not make
        // leaves nobody out: the event is delivered to everyone, which is the safe way to be wrong.
        $except = is_string($socket) ? (SocketId::connection($socket) ?? 0) : 0;

        $frames = [];
        foreach ($this->formatChannels($channels) as $channel) {
            $problem = ChannelName::problem($channel);
            if ($problem !== null) {
                throw new BroadcastException("Cannot broadcast to the channel '$channel': $problem");
            }
            $frames[$channel] = $this->frame((string) $event, $channel, $payload);
        }
        try {
            foreach ($frames as $channel => $frame) {
                $this->publisher->publish((string) $channel, $frame, false, $except);
            }
        } catch (PublishFailed $e) {
            throw new BroadcastException('wsrv publish failed: ' . $e->getMessage(), 0, $e);
        }
    }

    /** @param array<string, mixed> $payload */
    private function frame(string $event, string $channel, array $payload): string
    {
        try {
            if (EncryptedChannels::isEncrypted($channel)) {
                if ($this->encryption === null) {
                    // Never send the payload in the clear to a channel the browser expects to be encrypted.
                    throw new BroadcastException(
                        "Cannot broadcast to the encrypted channel '$channel': set encryption_master_key_base64 in the wsrv connection"
                    );
                }
                $data = $this->encryption->encrypt($channel, json_encode($payload, JSON_THROW_ON_ERROR));
            } else {
                $data = $payload;
            }
            return Frame::encode($event, $data, $channel);
        } catch (\JsonException $e) {
            throw new BroadcastException('The payload of the event cannot be encoded as JSON: ' . $e->getMessage(), 0, $e);
        }
    }

    /** @return array<mixed>|null the member's info as a JSON-able array, or null when there is none */
    private static function memberInfo(mixed $result): ?array
    {
        if (is_object($result)) {
            $result = json_decode(json_encode($result, JSON_THROW_ON_ERROR), true);
        }
        return is_array($result) && $result !== [] ? $result : null;
    }
}
