<?php
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Wsrv\Embedded;

use FFI;
use FFI\CData;
use Wsrv\ConnectionClosed;
use Wsrv\Ipc\Codec;
use Wsrv\Ipc\HandshakeRequest;
use Wsrv\WsrvException;

/**
 * The embedded server (Mode A, ADR-0025): libwsrv.so loaded through FFI, serving WebSockets from a
 * PHP CLI process. The library runs its own I/O threads; PHP code runs only here, on the calling
 * thread, when you call step() or run().
 *
 *     $server = new Server(['listen_port' => 8080, 'allow_origin' => ['https://app.example.com']]);
 *     $server->on('message', fn (Connection $c, string $data, bool $binary) => $c->send($data));
 *     $server->start();
 *     $server->run();
 *
 * Settings are the library's text keys (include/wsrv/wsrv.h): `listen_port`, `allow_origin`, ...
 * Booleans become "true"/"false", integers decimal text, a list repeats the key. The library checks
 * every value; a bad one makes start() throw with its message.
 *
 * Handlers (same names and arguments as Wsrv\Worker):
 *   open    fn(Connection $c, string $peerAddress)
 *   message fn(Connection $c, string $data, bool $binary)
 *   close   fn(Connection $c, int $code, string $reason, bool $clean)   the last event for $c
 *   error   fn(Connection $c, string $name)                            best effort, may be missing
 *   authenticate fn(Upgrade $u): bool|int|string|null   who may connect; needs the setting
 *           `handshake_hook` => true (and `handshake_forward_header` => ['Cookie'] to see cookies).
 *           Return true to accept, false for 403, an int status (400 401 403 404 429 500 503), a
 *           string (accept, choosing that subprotocol: it must be one the client offered), or null to
 *           answer later through `$u->accept()` / `$u->reject()` within `handshake_decision_timeout_ms`.
 *           FAIL CLOSED: a handler that throws (500, then the exception surfaces from step()) and an
 *           unusable return value (500, UnexpectedValueException) refuse the upgrade, never accept it.
 *           start() throws if the setting and the handler do not agree: a hook nobody answers would
 *           make every upgrade wait for the timeout, and a handler nobody asks would be a login check
 *           that never runs. Everything in `$u->request` comes from the browser: validate it.
 * Handlers run on the calling thread, one event at a time. An exception from a handler leaves
 * step()/run(); the server stays usable, so the caller can log it and call run() again.
 */
final class Server
{
    /** The header the cdef was made from (0.4). A library older than this lacks symbols; newer is additive. */
    public const ABI_MAJOR = 1;
    public const ABI_MINOR = 0;

    private const HANDLERS = ['open', 'message', 'close', 'error', 'authenticate'];

    // wsrv_status, wsrv_event_type (the cdef test checks these against the header).
    private const OK = 0;
    private const TIMEOUT = 1;
    private const ERR_NO_SUCH_CONN = 5;
    private const ERR_BACKPRESSURE = 6;
    private const ERR_INVALID_UTF8 = 7;
    private const EV_OPEN = 1;
    private const EV_TEXT = 2;
    private const EV_BINARY = 3;
    private const EV_CLOSE = 4;
    private const EV_ERROR = 5;
    private const EV_HANDSHAKE = 6;
    private const ERR_INVALID_ARGUMENT = 2;

    private readonly FFI $ffi;
    /** @var CData|null wsrv_server* */
    private ?CData $handle = null;
    private ?CData $event = null;
    private int $pid = 0;
    private bool $stopping = false;
    private bool $closed = false;
    /** @var array<string, callable> */
    private array $handlers = [];
    /** @var array<int, Connection> connections between their open and close events */
    private array $connections = [];

    /**
     * @param array<string, string|int|bool|list<string|int|bool>> $config the library's text keys
     * @param string|null $library path of libwsrv.so; default: the WSRV_LIBRARY environment
     *                             variable, then "libwsrv.so" for the dynamic loader
     */
    public function __construct(private readonly array $config = [], ?string $library = null)
    {
        if (!in_array(PHP_SAPI, ['cli', 'phpdbg'], true)) {
            throw new WsrvException(
                'the embedded server runs only in a PHP CLI process (this is ' . PHP_SAPI . '); '
                . 'for PHP-FPM use the sidecar: Wsrv\\Client with wsrvd'
            );
        }
        if (!extension_loaded('FFI')) {
            throw new WsrvException('the embedded server needs ext-ffi (or use the sidecar client)');
        }
        $path = $library ?? (getenv('WSRV_LIBRARY') ?: 'libwsrv.so');
        $cdef = file_get_contents(__DIR__ . '/wsrv.cdef');
        if ($cdef === false) {
            throw new WsrvException('cannot read ' . __DIR__ . '/wsrv.cdef');
        }
        try {
            $this->ffi = FFI::cdef($cdef, $path);
        } catch (FFI\Exception $e) {
            throw new WsrvException("cannot load $path through FFI: " . $e->getMessage(), 0, $e);
        }
        $this->checkAbi($path);
    }

    public function on(string $event, callable $handler): self
    {
        if (!in_array($event, self::HANDLERS, true)) {
            throw new \InvalidArgumentException(
                "unknown event '$event'; expected one of " . implode(', ', self::HANDLERS)
            );
        }
        $this->handlers[$event] = $handler;
        return $this;
    }

    /**
     * Create the native server from the settings and start listening. The library is created here
     * and not in the constructor so a script can fork() in between (wsrv.h: create after fork).
     */
    public function start(): void
    {
        $this->assertUsable();
        if ($this->handle !== null) {
            throw new WsrvException('the server is already started');
        }
        $this->checkHandshakeSettings();

        $error = $this->ffi->new('wsrv_error');
        $config = $this->ffi->wsrv_config_new();
        if ($config === null) {
            throw new WsrvException('out of memory creating the configuration');
        }
        try {
            foreach ($this->config as $key => $value) {
                foreach (is_array($value) ? $value : [$value] as $one) {
                    $status = $this->ffi->wsrv_config_set($config, (string) $key, self::text($one), FFI::addr($error));
                    $this->check($status, $error, "setting '$key'");
                }
            }
            $out = $this->ffi->new('wsrv_server*');
            $status = $this->ffi->wsrv_server_create($config, FFI::addr($out), FFI::addr($error));
            $this->check($status, $error, 'creating the server');
        } finally {
            $this->ffi->wsrv_config_free($config);
        }
        $this->handle = $out;
        $this->pid = getmypid();
        $this->event = $this->ffi->new('wsrv_event');
        try {
            $status = $this->ffi->wsrv_server_start($this->handle, FFI::addr($error));
            $this->check($status, $error, 'starting the server');
        } catch (\Throwable $e) {
            $this->destroyNative();
            throw $e;
        }
    }

    /** The port actually bound (useful with `listen_port` 0). */
    public function port(): int
    {
        return $this->ffi->wsrv_server_port($this->native());
    }

    /**
     * The descriptor that is readable while events wait, for another event loop (ReactPHP, Revolt,
     * stream_select). Do not read it or close it; call step(0) until it returns false.
     */
    public function eventFd(): int
    {
        return $this->ffi->wsrv_server_event_fd($this->native());
    }

    /** Connections currently open (as far as this server has told the handlers). */
    public function connectionCount(): int
    {
        return count($this->connections);
    }

    /**
     * Wait up to `$timeoutMs` for one event and run its handler.
     *
     * @return bool true when an event was handled, false on timeout
     */
    public function step(int $timeoutMs = 100): bool
    {
        $server = $this->native();
        $event = $this->event ?? throw new WsrvException('the server is not started');
        // wsrv.h: zero the event and set struct_size BEFORE every poll, so a release is always safe.
        FFI::memset(FFI::addr($event), 0, FFI::sizeof($event));
        $event->struct_size = FFI::sizeof($event);
        $status = $this->ffi->wsrv_poll($server, FFI::addr($event), max(0, $timeoutMs));
        if ($status === self::TIMEOUT) {
            return false;
        }
        $this->check($status, null, 'polling');
        try {
            // Copy everything out before releasing: nothing may point into library memory afterwards.
            $type = $event->type;
            $id = (int) $event->conn;
            $data = $event->len > 0 ? FFI::string($event->data, $event->len) : '';
            $code = $event->close_code;
            $clean = $event->clean !== 0;
            $request = $type === self::EV_HANDSHAKE ? $this->copyHandshake($event->handshake) : null;
        } finally {
            $this->ffi->wsrv_event_release(FFI::addr($event));
        }
        $this->dispatch($type, $id, $data, $code, $clean, $request);
        return true;
    }

    /** Handle events until stop() is called (from a handler or a signal handler). */
    public function run(): void
    {
        $this->stopping = false;
        while (!$this->stopping) {
            $this->step(100);
        }
    }

    /** Make run() return after the event being handled. Safe from a pcntl signal handler. */
    public function stop(): void
    {
        $this->stopping = true;
    }

    /** Stop accepting, close the connections gracefully and free the native server. Idempotent. */
    public function close(): void
    {
        $this->stopping = true;
        $this->destroyNative();
    }

    public function __destruct()
    {
        $this->destroyNative();
    }

    /** @internal used by Connection */
    public function sendText(int $id, string $text): bool
    {
        $status = $this->ffi->wsrv_send_text($this->native(), $id, $text, strlen($text));
        return $this->commandResult($status, $id);
    }

    /** @internal used by Connection */
    public function sendBinary(int $id, string $data): bool
    {
        $length = strlen($data);
        // `const uint8_t *` does not take a PHP string (FFI converts only char*), so copy once.
        // (An array passed as an argument decays to a pointer; FFI::cast would reinterpret its BYTES as
        // an address and crash.)
        $buffer = $this->ffi->new('uint8_t[' . max(1, $length) . ']');
        if ($length > 0) {
            FFI::memcpy($buffer, $data, $length);
        }
        $status = $this->ffi->wsrv_send_binary($this->native(), $id, $buffer, $length);
        return $this->commandResult($status, $id);
    }

    /** @internal used by Connection */
    public function closeConnection(int $id, int $code, string $reason): void
    {
        $status = $this->ffi->wsrv_close($this->native(), $id, $code, $reason, strlen($reason));
        if ($status === self::ERR_BACKPRESSURE) {
            throw new WsrvException('close was not accepted: a queue is full; try again');
        }
        $this->commandResult($status, $id);
    }

    // ---- internals ---------------------------------------------------------------------------

    private function dispatch(int $type, int $id, string $data, int $code, bool $clean, ?HandshakeRequest $request): void
    {
        $connection = $this->connections[$id] ?? null;
        switch ($type) {
            case self::EV_OPEN:
                $connection = $this->connections[$id] = new Connection($this, $id, $data);
                $this->call('open', $connection, $data);
                return;
            case self::EV_TEXT:
            case self::EV_BINARY:
                $connection ??= $this->connections[$id] = new Connection($this, $id);
                $this->call('message', $connection, $data, $type === self::EV_BINARY);
                return;
            case self::EV_CLOSE:
                $connection ??= new Connection($this, $id);
                try {
                    $this->call('close', $connection, $code, $data, $clean);
                } finally {
                    unset($this->connections[$id]);
                }
                return;
            case self::EV_ERROR:
                $connection ??= new Connection($this, $id);
                $this->call('error', $connection, $data);
                return;
            case self::EV_HANDSHAKE:
                if ($request === null) {
                    $this->refuseQuietly($id, 500);
                    throw new WsrvException('a handshake event carried no request');
                }
                $this->authenticate($id, $request);
                return;
            default:
                // A newer library may add event types (additive ABI): ignoring one is safe only
                // because every type this binding enables is handled above.
                return;
        }
    }

    private function call(string $event, mixed ...$arguments): void
    {
        if (isset($this->handlers[$event])) {
            ($this->handlers[$event])(...$arguments);
        }
    }

    /** `true` accepted, `false` backpressure; everything else is an exception. */
    private function commandResult(int $status, int $id): bool
    {
        return match ($status) {
            self::OK => true,
            self::ERR_BACKPRESSURE => false,
            self::ERR_NO_SUCH_CONN => throw new ConnectionClosed("connection $id has ended"),
            self::ERR_INVALID_UTF8 => throw new \InvalidArgumentException('text is not valid UTF-8; nothing was sent'),
            default => throw new WsrvException($this->statusName($status) . " sending to connection $id"),
        };
    }

    /** @param CData|null $error wsrv_error whose message is used when set */
    private function check(int $status, ?CData $error, string $what): void
    {
        if ($status === self::OK) {
            return;
        }
        $message = $error !== null ? self::string($error->message) : '';
        throw new WsrvException(
            "$what failed: " . $this->statusName($status) . ($message !== '' ? ": $message" : '')
        );
    }

    private function statusName(int $status): string
    {
        $name = self::string($this->ffi->wsrv_status_name($status));
        return $name === '' ? "status $status" : $name;
    }

    /** A C string as PHP gives it: char arrays in structs arrive as strings, char* as CData (or null). */
    private static function string(mixed $value): string
    {
        return match (true) {
            is_string($value) => $value,
            $value instanceof CData => FFI::string($value),
            default => '',
        };
    }

    private function checkAbi(string $path): void
    {
        $version = (int) $this->ffi->wsrv_abi_version();
        $major = $version >> 16;
        $minor = $version & 0xFFFF;
        if ($major !== self::ABI_MAJOR || $minor < self::ABI_MINOR) {
            throw new WsrvException(
                "$path has C ABI $major.$minor; this binding needs " . self::ABI_MAJOR . '.' . self::ABI_MINOR
                . ' or a newer minor of the same major'
            );
        }
    }

    /** The setting and the handler must agree (see the class comment); a mismatch fails at start(). */
    private function checkHandshakeSettings(): void
    {
        $hook = self::isTrue($this->config['handshake_hook'] ?? false);
        $handler = isset($this->handlers['authenticate']);
        if ($hook && !$handler) {
            throw new WsrvException(
                'handshake_hook is on but no authenticate handler is registered: every upgrade would wait '
                . 'for an answer nobody gives (register one with on(\'authenticate\', ...) before start())'
            );
        }
        if (!$hook && $handler) {
            throw new WsrvException(
                'an authenticate handler is registered but handshake_hook is off: no upgrade would ever ask '
                . 'it, so the login check would never run (set \'handshake_hook\' => true)'
            );
        }
    }

    private static function isTrue(mixed $value): bool
    {
        return $value === true || $value === 1 || $value === '1' || $value === 'true';
    }

    /** Copy a wsrv_handshake into PHP values. Runs before the event is released. */
    private function copyHandshake(?CData $h): ?HandshakeRequest
    {
        if ($h === null) {
            return null;
        }
        $str = static fn (CData $s): string => $s->len > 0 ? FFI::string($s->data, $s->len) : '';
        $offered = [];
        for ($i = 0; $i < $h->offered_count; $i++) {
            $offered[] = $str($h->offered[$i]);
        }
        $headers = [];
        for ($i = 0; $i < $h->header_count; $i++) {
            $headers[] = [$str($h->headers[$i]->name), $str($h->headers[$i]->value)];
        }
        return new HandshakeRequest(
            $str($h->peer), $str($h->path), $str($h->query), $str($h->origin),
            $str($h->host), $str($h->subprotocol), $offered, $headers,
        );
    }

    /** A browser's upgrade waits for us. Every path that does not accept, refuses. */
    private function authenticate(int $id, HandshakeRequest $request): void
    {
        $upgrade = new Upgrade($this, $id, $request);
        if (!isset($this->handlers['authenticate'])) {
            $this->refuseQuietly($id, 503);  // start() prevents this; stay safe
            return;
        }
        try {
            $decision = ($this->handlers['authenticate'])($upgrade);
            if (!$upgrade->answered()) {
                $this->apply($upgrade, $decision);
            }
        } catch (\Throwable $e) {
            if (!$upgrade->answered()) {
                $this->refuseQuietly($id, 500);
            }
            throw $e;
        }
    }

    private function apply(Upgrade $upgrade, mixed $decision): void
    {
        if ($decision === null) {
            return;  // the handler answers later, through $upgrade
        }
        if ($decision === true) {
            $upgrade->accept();
        } elseif ($decision === false) {
            $upgrade->reject(403);
        } elseif (is_int($decision) && in_array($decision, Codec::REJECT_STATUSES, true)) {
            $upgrade->reject($decision);
        } elseif (is_string($decision) && in_array($decision, $upgrade->request->offered, true)) {
            $upgrade->accept($decision);
        } else {
            throw new \UnexpectedValueException(
                'authenticate must return true, false, null, one of ' . implode(', ', Codec::REJECT_STATUSES)
                . ', or a subprotocol the client offered; got ' . get_debug_type($decision)
            );
        }
    }

    /** Best effort, for failure paths: the original problem is what the caller should see. */
    private function refuseQuietly(int $id, int $status): void
    {
        try {
            $this->rejectHandshake($id, $status);
        } catch (ConnectionClosed) {
            // the library no longer waits (timeout, or the client left): nothing to refuse
        }
    }

    /** @internal used by Upgrade */
    public function acceptHandshake(int $id, string $subprotocol): void
    {
        $status = $this->ffi->wsrv_handshake_accept(
            $this->native(), $id, $subprotocol === '' ? null : $subprotocol, strlen($subprotocol)
        );
        $this->handshakeResult($status, $id);
    }

    /** @internal used by Upgrade */
    public function rejectHandshake(int $id, int $status): void
    {
        $this->handshakeResult($this->ffi->wsrv_handshake_reject($this->native(), $id, $status), $id);
    }

    private function handshakeResult(int $status, int $id): void
    {
        match ($status) {
            self::OK => null,
            self::ERR_NO_SUCH_CONN => throw new ConnectionClosed(
                "upgrade $id is no longer waiting (answered, timed out, or the client left)"
            ),
            self::ERR_INVALID_ARGUMENT => throw new \InvalidArgumentException(
                "the library refused the answer for upgrade $id (a subprotocol not offered, or a status it does not send)"
            ),
            default => throw new WsrvException($this->statusName($status) . " answering upgrade $id"),
        };
    }

    private static function text(string|int|bool $value): string
    {
        return is_bool($value) ? ($value ? 'true' : 'false') : (string) $value;
    }

    private function assertUsable(): void
    {
        if ($this->closed) {
            throw new WsrvException('the server was closed');
        }
        if ($this->pid !== 0 && $this->pid !== getmypid()) {
            throw new WsrvException('the server was created in another process (fork): create it after fork()');
        }
    }

    private function native(): CData
    {
        $this->assertUsable();
        return $this->handle ?? throw new WsrvException('the server is not started');
    }

    private function destroyNative(): void
    {
        if ($this->handle === null || $this->closed) {
            return;
        }
        $this->closed = true;
        if ($this->pid === getmypid()) {
            $this->ffi->wsrv_server_stop($this->handle);
            $this->ffi->wsrv_server_destroy($this->handle);
        }
        $this->handle = null;
        $this->event = null;
        $this->connections = [];
    }
}
