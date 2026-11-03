<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Ipc;

use Wsrv\ProtocolError;

/**
 * Turns the byte stream from the daemon into messages. Incremental: feed it whatever a read
 * returned (one byte or ten frames) and ask for messages until it returns null.
 *
 * Strict, like the C++ decoder: the declared length is checked BEFORE the body is awaited, so a
 * confused peer cannot make us collect a huge body; unknown types, non-zero flags, wrong body
 * sizes and inconsistent events are errors. After an error the decoder stays failed.
 */
final class FrameDecoder
{
    private string $buffer = '';
    private int $offset = 0;
    private ?ProtocolError $failure = null;

    /** @param int $maxFrame the largest accepted value of the length field */
    public function __construct(private int $maxFrame)
    {
    }

    public function setMaxFrame(int $maxFrame): void
    {
        $this->maxFrame = $maxFrame;
    }

    public function feed(string $bytes): void
    {
        $this->buffer .= $bytes;
    }

    /** Bytes held but not yet turned into a message. */
    public function pending(): int
    {
        return strlen($this->buffer) - $this->offset;
    }

    /**
     * @return HelloOk|Event|StatusReply|Pong|null the next message, or null when more bytes are needed
     * @throws ProtocolError
     */
    public function next(): HelloOk|Event|StatusReply|Pong|null
    {
        if ($this->failure !== null) {
            throw $this->failure;
        }
        try {
            return $this->parseNext();
        } catch (ProtocolError $e) {
            $this->failure = $e;
            throw $e;
        }
    }

    private function parseNext(): HelloOk|Event|StatusReply|Pong|null
    {
        if ($this->pending() < Codec::HEADER_SIZE) {
            return null;
        }
        /** @var array{length:int, type:int, flags:int} $head */
        $head = unpack('Nlength/Ctype/Cflags', $this->buffer, $this->offset);
        $length = $head['length'];
        if ($length < 2) {
            throw new ProtocolError('frame length ' . $length . ' is too short');
        }
        if ($length > $this->maxFrame) {
            throw new ProtocolError('frame length ' . $length . ' exceeds the limit ' . $this->maxFrame);
        }
        if ($head['flags'] !== 0) {
            throw new ProtocolError('reserved flags are not zero');
        }
        if ($this->pending() < 4 + $length) {
            return null;
        }
        $body = substr($this->buffer, $this->offset + Codec::HEADER_SIZE, $length - 2);
        $this->offset += 4 + $length;
        if ($this->offset > 65536 && $this->offset * 2 > strlen($this->buffer)) {
            $this->buffer = substr($this->buffer, $this->offset);  // drop what was consumed
            $this->offset = 0;
        }
        return $this->message($head['type'], $body);
    }

    private function message(int $type, string $body): HelloOk|Event|StatusReply|Pong
    {
        switch ($type) {
            case Codec::TYPE_HELLO_OK:
                if (strlen($body) !== 8) {
                    throw new ProtocolError('HELLO_OK has the wrong size');
                }
                /** @var array{major:int, minor:int, max:int} $f */
                $f = unpack('nmajor/nminor/Nmax', $body);
                return new HelloOk($f['major'], $f['minor'], $f['max']);
            case Codec::TYPE_STATUS:
                if (strlen($body) !== 5) {
                    throw new ProtocolError('STATUS has the wrong size');
                }
                /** @var array{request:int, status:int} $f */
                $f = unpack('Nrequest/Cstatus', $body);
                return new StatusReply($f['request'], $f['status']);
            case Codec::TYPE_PONG:
                if ($body !== '') {
                    throw new ProtocolError('PONG must be empty');
                }
                return new Pong();
            case Codec::TYPE_EVENT:
                return $this->event($body);
            default:
                // Includes the types only an application may send: a daemon never sends them.
                throw new ProtocolError('unexpected frame type ' . $type . ' from the daemon');
        }
    }

    private function event(string $body): Event
    {
        if (strlen($body) < 12) {
            throw new ProtocolError('EVENT is too short');
        }
        /** @var array{kind:int, conn:int, code:int, clean:int} $f */
        $f = unpack('Ckind/Jconn/ncode/Cclean', $body);
        $kind = EventKind::tryFrom($f['kind']);
        if ($kind === null) {
            throw new ProtocolError('EVENT has an unknown kind ' . $f['kind']);
        }
        if ($f['conn'] < 1) {
            // 0 is never a connection; a negative value means 2^63 or more, which PHP cannot hold
            // (the daemon counts up from 1, so this is a corrupt stream, not a busy server).
            throw new ProtocolError('EVENT has an invalid connection id');
        }
        if ($f['clean'] > 1) {
            throw new ProtocolError('EVENT clean flag must be 0 or 1');
        }
        if ($kind !== EventKind::Close && ($f['code'] !== 0 || $f['clean'] !== 0)) {
            throw new ProtocolError('only a Close event carries a close code');
        }
        return new Event($kind, $f['conn'], $f['code'], $f['clean'] === 1, substr($body, 12));
    }
}
