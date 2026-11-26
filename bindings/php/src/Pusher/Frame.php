<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Pusher;

/**
 * One Pusher protocol message: `{"event": ..., "data": ..., "channel": ...}`.
 *
 * Everything from a browser is untrusted, so decode() bounds the size and the nesting before and
 * while it parses, and answers every defect with BadFrame, never with a PHP warning or a different
 * exception. `data` is kept as the browser sent it: pusher-js sends an object for subscribe and
 * client events, other senders a JSON string; object() gives the structured form of either.
 */
final class Frame
{
    private const MAX_DEPTH = 8;
    private const MAX_EVENT_BYTES = 200;
    private const FLAGS = JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR;

    private function __construct(
        public readonly string $event,
        public readonly ?string $channel,
        private readonly mixed $data,
        private readonly string $text,
    ) {
    }

    /** @throws BadFrame */
    public static function decode(string $text, int $maxBytes): self
    {
        if ($text === '' || strlen($text) > $maxBytes) {
            throw new BadFrame('frame is empty or larger than ' . $maxBytes . ' bytes');
        }
        try {
            $value = json_decode($text, true, self::MAX_DEPTH, JSON_THROW_ON_ERROR);
        } catch (\JsonException) {
            throw new BadFrame('frame is not valid JSON (or nests too deeply)');
        }
        if (!is_array($value) || array_is_list($value)) {
            throw new BadFrame('frame is not a JSON object');
        }
        $event = $value['event'] ?? null;
        if (!is_string($event) || $event === '' || strlen($event) > self::MAX_EVENT_BYTES) {
            throw new BadFrame('frame has no usable event name');
        }
        $channel = $value['channel'] ?? null;
        if ($channel !== null && !is_string($channel)) {
            throw new BadFrame('frame channel is not a string');
        }
        return new self($event, $channel, $value['data'] ?? null, $text);
    }

    /** The data as a JSON array or object, whether it arrived as one or as a JSON string; else null. */
    public function object(): ?array
    {
        if (is_array($this->data)) {
            return $this->data;
        }
        if (is_string($this->data) && $this->data !== '' && ($this->data[0] === '{' || $this->data[0] === '[')) {
            try {
                $decoded = json_decode($this->data, true, self::MAX_DEPTH, JSON_THROW_ON_ERROR);
            } catch (\JsonException) {
                return null;
            }
            return is_array($decoded) ? $decoded : null;
        }
        return null;
    }

    /**
     * The data of a client event, ready to be sent on to other browsers: decoded so that an empty
     * object stays an object (object() would turn `{}` and `[]` into the same empty array), whether
     * it arrived as JSON or as a JSON string. Null if it is not an object or an array.
     */
    public function relayData(): \stdClass|array|null
    {
        // The text decoded once already (with a depth bound), so this cannot fail.
        $decoded = json_decode($this->text, false, self::MAX_DEPTH);
        $data = is_object($decoded) ? ($decoded->data ?? null) : null;
        if (is_string($data) && $data !== '' && ($data[0] === '{' || $data[0] === '[')) {
            $data = json_decode($data, false, self::MAX_DEPTH);
        }
        return (is_object($data) || is_array($data)) ? $data : null;
    }

    /**
     * A client event on its way to the other members. The server decides every field except `data`
     * (the browser's own, relayed as an object): the sender cannot add or change the rest.
     *
     * @throws \JsonException only for a value that is not encodable, which is a bug in the caller
     */
    public static function relay(string $event, string $channel, \stdClass|array $data, ?string $userId): string
    {
        $frame = ['event' => $event, 'channel' => $channel, 'data' => $data];
        if ($userId !== null) {
            $frame['user_id'] = $userId;
        }
        return json_encode($frame, self::FLAGS);
    }

    /**
     * A frame for a browser. `data` is itself JSON text inside the JSON, which is how Pusher servers
     * send it and what pusher-js parses.
     *
     * @throws \JsonException only for a value that is not encodable, which is a bug in the caller
     */
    public static function encode(string $event, mixed $data = null, ?string $channel = null): string
    {
        $frame = ['event' => $event];
        if ($data !== null) {
            $frame['data'] = json_encode($data, self::FLAGS);
        }
        if ($channel !== null) {
            $frame['channel'] = $channel;
        }
        return json_encode($frame, self::FLAGS);
    }
}
