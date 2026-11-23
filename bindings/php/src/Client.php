<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv;

use Wsrv\Ipc\Codec;
use Wsrv\Ipc\Event;
use Wsrv\Ipc\FrameDecoder;
use Wsrv\Ipc\HelloOk;
use Wsrv\Ipc\Pong;
use Wsrv\Ipc\StatusReply;

/**
 * One connection to the wsrvd sidecar over its Unix socket (docs/IPC_PROTOCOL.md).
 *
 * The daemon is the only place with WebSocket logic; this class only frames bytes. Typical use:
 *
 *     $client = Client::connect('/run/wsrv/ipc.sock', token: $secret);
 *     while (($m = $client->poll(1.0)) !== null) { ... }   // Event | StatusReply | Pong
 *     $request = $client->sendText($conn, 'hello');        // the STATUS for $request arrives later
 *
 * Reading continuously matters: the daemon slows the browsers down when this process reads slowly
 * and, after `ipc_stall_ms` of reading nothing, closes the link. For that reason sending never
 * blocks on a daemon that is waiting for US: while a write is pending, whatever the daemon sends is
 * read and queued, and poll() returns it later (see writeAll()).
 */
final class Client
{
    /** @var resource|null */
    private $stream;
    private FrameDecoder $decoder;
    /** @var list<HelloOk|Event|StatusReply|Pong> messages read while writing, not yet handed out */
    private array $queue = [];
    private int $nextRequest = 1;
    private int $maxFrame;
    private float $writeTimeout = 30.0;

    /** @param resource $stream */
    private function __construct($stream, int $maxFrame, FrameDecoder $decoder)
    {
        $this->stream = $stream;
        $this->maxFrame = $maxFrame;
        $this->decoder = $decoder;
    }

    /**
     * Connect and complete the HELLO handshake.
     *
     * @throws HandshakeRefused the daemon said no (wrong token or version, or another application is connected)
     * @throws ConnectionClosed the daemon hung up, or never answered within $timeout seconds
     */
    public static function connect(string $socketPath, string $token = '', float $timeout = 5.0): self
    {
        $stream = @stream_socket_client('unix://' . $socketPath, $errno, $error, $timeout);
        if ($stream === false) {
            throw new WsrvException('cannot connect to ' . $socketPath . ': ' . $error . ' (' . $errno . ')');
        }
        return self::handshake($stream, $token, $timeout);
    }

    /**
     * Run the HELLO handshake on a stream that is already connected. connect() uses this; tests and
     * embedders with their own socket can too. The client takes ownership of $stream.
     *
     * @param resource $stream
     */
    public static function handshake($stream, string $token = '', float $timeout = 5.0): self
    {
        stream_set_blocking($stream, false);
        // Before HELLO_OK the only legal replies are tiny; do not let the peer make us collect more.
        $decoder = new FrameDecoder(1024);
        $client = new self($stream, 1024, $decoder);
        try {
            try {
                $client->writeAll(Codec::hello($token), $timeout);
            } catch (ConnectionClosed $closed) {
                // A daemon that refuses us says so and hangs up at once, possibly before our HELLO
                // is written. What it said is already in the queue: report that, not "closed".
                $said = $client->queue[0] ?? null;
                throw $said instanceof StatusReply ? new HandshakeRefused($said->status) : $closed;
            }
            $message = $client->next(microtime(true) + $timeout);
            if ($message instanceof HelloOk && $message->major === Codec::VERSION_MAJOR) {
                $client->maxFrame = $message->maxFrame;
                $decoder->setMaxFrame($message->maxFrame);
                return $client;
            }
            if ($message instanceof StatusReply) {
                throw new HandshakeRefused($message->status);
            }
            if ($message === null) {
                throw new ConnectionClosed('no HELLO_OK within ' . $timeout . ' seconds');
            }
            throw new ProtocolError(
                $message instanceof HelloOk
                    ? 'daemon speaks protocol major ' . $message->major
                    : 'unexpected message before HELLO_OK'
            );
        } catch (\Throwable $e) {
            $client->close();  // never leak the socket on a failed handshake
            throw $e;
        }
    }

    /** How long a command may wait for the daemon to accept its bytes (default 30 s). */
    public function setWriteTimeout(float $seconds): void
    {
        $this->writeTimeout = $seconds;
    }

    /** The largest frame the daemon accepts and sends (the length field), from HELLO_OK. */
    public function maxFrame(): int
    {
        return $this->maxFrame;
    }

    /**
     * Wait up to $timeout seconds for the next message from the daemon.
     *
     * @return Event|StatusReply|Pong|null null on timeout
     * @throws ConnectionClosed
     * @throws ProtocolError
     */
    public function poll(float $timeout = 0.0): Event|StatusReply|Pong|null
    {
        $message = $this->next(microtime(true) + $timeout);
        return $message instanceof HelloOk ? throw new ProtocolError('second HELLO_OK') : $message;
    }

    /** @return int the request id; the daemon answers it with a StatusReply carrying the same id */
    public function sendText(int $conn, string $text): int
    {
        return $this->command(fn (int $r): string => Codec::sendText($r, $conn, $text));
    }

    /** @return int the request id */
    public function sendBinary(int $conn, string $data): int
    {
        return $this->command(fn (int $r): string => Codec::sendBinary($r, $conn, $data));
    }

    /** Start the WebSocket closing handshake on $conn. @return int the request id */
    public function closeConnection(int $conn, int $code = 1000, string $reason = ''): int
    {
        return $this->command(fn (int $r): string => Codec::close($r, $conn, $code, $reason));
    }

    /**
     * Answer a Handshake event: let the upgrade in (ADR-0018). Prefer Upgrade::accept().
     *
     * @return int the request id
     */
    public function acceptHandshake(int $conn, string $subprotocol = ''): int
    {
        return $this->command(fn (int $r): string => Codec::handshakeAccept($r, $conn, $subprotocol));
    }

    /** Refuse an upgrade with 400, 401, 403, 404, 429, 500 or 503. Prefer Upgrade::reject(). @return int the request id */
    public function rejectHandshake(int $conn, int $status): int
    {
        return $this->command(fn (int $r): string => Codec::handshakeReject($r, $conn, $status));
    }

    /**
     * Put one of your connections in a room (ADR-0028). The StatusReply says OK, NO_SUCH_CONN (not
     * one of yours, or closed), LIMIT (a broker bound; waiting will not help until someone leaves) or
     * INVALID_ARGUMENT (the daemon runs without `broker = true`). @return int the request id
     */
    public function subscribe(int $conn, string $topic): int
    {
        return $this->command(fn (int $r): string => Codec::subscribe($r, $conn, $topic));
    }

    /** @return int the request id */
    public function unsubscribe(int $conn, string $topic): int
    {
        return $this->command(fn (int $r): string => Codec::unsubscribe($r, $conn, $topic));
    }

    /**
     * Send to every subscriber of $topic except connection $except. OK means the broker took it, not
     * how many received it (the HTTP publish endpoint reports counts). @return int the request id
     */
    public function publish(string $topic, string $payload, bool $binary = false, int $except = 0): int
    {
        return $this->command(fn (int $r): string => Codec::publish($r, $topic, $payload, $binary, $except));
    }

    /** The daemon answers with a Pong message. */
    public function ping(): void
    {
        $this->writeAll(Codec::ping(), 5.0);
    }

    public function close(): void
    {
        if ($this->stream !== null) {
            fclose($this->stream);
            $this->stream = null;
        }
    }

    public function __destruct()
    {
        $this->close();
    }

    /** @param callable(int):string $encode */
    private function command(callable $encode): int
    {
        $request = $this->nextRequest;
        // Request ids run 1..2^32-1 and wrap to 1 (0 is reserved for the handshake).
        $this->nextRequest = $request === 0xFFFFFFFF ? 1 : $request + 1;
        $frame = $encode($request);
        if (strlen($frame) - 4 > $this->maxFrame) {
            throw new ProtocolError('message is larger than the daemon accepts (' . $this->maxFrame . ' bytes per frame)');
        }
        $this->writeAll($frame, $this->writeTimeout);
        return $request;
    }

    private function next(float $deadline): HelloOk|Event|StatusReply|Pong|null
    {
        for (;;) {
            if ($this->queue !== []) {
                return array_shift($this->queue);
            }
            $message = $this->decoder->next();
            if ($message !== null) {
                return $message;
            }
            $left = $deadline - microtime(true);
            if ($left < 0) {
                return null;
            }
            $this->waitReadable($left);
            $this->readAvailable();
        }
    }

    private function waitReadable(float $seconds): void
    {
        $read = [$this->stream()];
        $write = null;
        $except = null;
        $sec = (int) floor($seconds);
        @stream_select($read, $write, $except, $sec, (int) (($seconds - $sec) * 1_000_000));
    }

    /** Read what is there and feed the decoder. @throws ConnectionClosed on EOF */
    private function readAvailable(): void
    {
        $stream = $this->stream();
        $data = fread($stream, 65536);
        if ($data === false || ($data === '' && feof($stream))) {
            $this->close();
            throw new ConnectionClosed('wsrvd closed the connection');
        }
        if ($data !== '') {
            $this->decoder->feed($data);
        }
    }

    /** Move whatever the daemon already sent into the queue; an EOF here is not an error yet. */
    private function takeWhatWasSaid(): void
    {
        try {
            $this->readAvailable();
        } catch (ConnectionClosed) {
            // the stream is gone; the bytes read before it ended are in the decoder
        }
        while (($message = $this->decoder->next()) !== null) {
            $this->queue[] = $message;
        }
    }

    /**
     * Write all of $bytes. If the daemon is not reading (its output to us is full, so it stopped
     * reading commands), reading from it is what unblocks it: we read and queue its messages while
     * the write waits. Without this, a worker that answers a burst of events with a burst of
     * commands would deadlock against a daemon that waits for it to read.
     */
    private function writeAll(string $bytes, float $timeout): void
    {
        $stream = $this->stream();
        $deadline = microtime(true) + $timeout;
        while ($bytes !== '') {
            $written = @fwrite($stream, $bytes);
            if ($written === false) {
                $this->takeWhatWasSaid();  // the daemon may have told us why before it hung up
                $this->close();
                throw new ConnectionClosed('wsrvd closed the connection');
            }
            if ($written > 0) {
                $bytes = substr($bytes, $written);
                continue;
            }
            $left = $deadline - microtime(true);
            if ($left <= 0) {
                throw new ConnectionClosed('wsrvd did not accept our data within ' . $timeout . ' seconds');
            }
            $read = [$stream];
            $write = [$stream];
            $except = null;
            $sec = (int) floor($left);
            if (@stream_select($read, $write, $except, $sec, (int) (($left - $sec) * 1_000_000)) > 0 && $read !== []) {
                $this->readAvailable();
                while (($m = $this->decoder->next()) !== null) {
                    $this->queue[] = $m;
                }
            }
        }
    }

    /** @return resource */
    private function stream()
    {
        if ($this->stream === null) {
            throw new ConnectionClosed('the connection is closed');
        }
        return $this->stream;
    }
}
