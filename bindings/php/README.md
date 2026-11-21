# wsrv/client: PHP client for the `wsrvd` WebSocket sidecar

A pure-PHP client (PHP 8.1+, plain streams, no extensions needed) for the **wsrvd** daemon. The daemon owns the WebSocket connections, framing, UTF-8 checks, limits and backpressure; your PHP code only receives events and sends commands over a local Unix socket. That is what makes it work on every PHP stack, including PHP-FPM workers and Laravel/Symfony queue workers: PHP never holds a WebSocket or a thread.

Protocol: [`docs/IPC_PROTOCOL.md`](../../docs/IPC_PROTOCOL.md). Lesson: [`docs/learning/17-the-php-sidecar-client.md`](../../docs/learning/17-the-php-sidecar-client.md).

## Quick start (about five minutes)

1. Start the daemon (the socket directory must be private, mode `0700`, and yours):

   ```sh
   mkdir -m 700 /tmp/wsrv
   wsrvd --ipc-socket=/tmp/wsrv/ipc.sock --listen-port=8080 --allow-origin=http://localhost:8000
   ```

2. Write a worker (`examples/echo.php` is this, complete):

   ```php
   use Wsrv\{Client, Connection, Worker};

   $worker = new Worker(Client::connect('/tmp/wsrv/ipc.sock'));
   $worker->on('message', fn (Connection $c, string $data, bool $binary) =>
       $binary ? $c->sendBinary($data) : $c->send($data));
   $worker->run();
   ```

3. Connect a WebSocket client to `ws://127.0.0.1:8080/` and send something. The daemon enforces the
   `Origin` policy (RFC 6455 section 10.2): a browser page served from `http://localhost:8000` is
   accepted; a command-line client such as `websocat` or `wscat` sends **no** `Origin` header and gets
   `403 Forbidden` unless the daemon was started with `--allow-missing-origin=true` (fine for local
   experiments; think before using it on a browser-facing deployment).

## API

| | |
|---|---|
| `Client::connect($socket, $token = '', $timeout = 5.0)` | connect and run the HELLO handshake |
| `$client->poll($timeout)` | next `Event` / `StatusReply` / `Pong`, or `null` on timeout |
| `$client->sendText($conn, $text)`, `sendBinary`, `closeConnection($conn, $code, $reason)` | commands; return the request id |
| `Worker::on('open'\|'message'\|'close'\|'error'\|'status'\|'authenticate', $fn)` | callbacks (signatures in `src/Worker.php`) |
| `Worker::run()` / `step($timeout)` / `stop()` | blocking loop, or one step for your own loop |
| `Connection::send/sendBinary/close` | handle given to callbacks |
| `Upgrade::accept($subprotocol = null)` / `reject($status = 403)` | answer a waiting upgrade (`authenticate`) |
| `HandshakeRequest::cookie($name)` / `header($name)` / `headers($name)` | read the request in `authenticate` |

A command's request id comes back later in a `StatusReply` (`'status'` handler). `StatusCode::OK` means *accepted*, not delivered. Ids number from 1; id 0 is reserved.

## Authenticating connections

Turn the hook on in the daemon (`--handshake-hook=true --handshake-forward-header=Cookie`) and register an `authenticate` handler. No browser gets a WebSocket until it answers:

```php
$worker->on('authenticate', fn (Upgrade $u) => $u->request->cookie('session') === lookup_session());
```

Return `true` (accept), `false` (403), an int status (`400, 401, 403, 404, 429, 500, 503`), a string (accept, choosing that subprotocol), or `null` to answer later with `$u->accept()` / `$u->reject()`. **It fails closed:** no handler, a handler that throws, an unusable return value, or no answer within the daemon's `handshake_decision_timeout_ms` (default 3 s) all refuse the browser (500 or 503), never accept. The daemon also refuses at once while your worker is not connected. Everything the request carries (`$u->request`) comes from the browser: validate it. See `examples/authenticate.php` and `docs/OPERATIONS.md`.

## Embedded mode (FFI, PHP CLI only)

No daemon: `Wsrv\Embedded\Server` loads `libwsrv.so` through `ext-ffi` and serves WebSockets from your own CLI process. The library's threads do the sockets; your handlers run on your thread when you call `run()` or `step()`. Handler names and arguments are the same as `Worker`'s, so moving between the modes changes the constructor, not the handlers. Design: [ADR-0025](../../docs/adr/0025-php-ffi-binding.md); example `examples/embedded_echo.php`.

```php
use Wsrv\Embedded\{Connection, Server};

$server = new Server(['listen_port' => 8080, 'allow_origin' => ['https://app.example.com']]);
$server->on('message', fn (Connection $c, string $data, bool $binary) => $binary ? $c->sendBinary($data) : $c->send($data));
$server->start();
$server->run();
```

- Settings are the library's text keys (`include/wsrv/wsrv.h`); a bad value makes `start()` throw with the library's message.
- The library is found through the `WSRV_LIBRARY` environment variable or the second constructor argument. A library with a different ABI major, or an older minor than `Server::ABI_MINOR`, is refused with both versions named.
- `send()` returns `false` when a queue is full (backpressure; nothing was queued) and throws `ConnectionClosed` once the connection has ended.
- It refuses PHP-FPM and any SAPI but `cli`/`phpdbg`. Create the server after `fork()`. `handshake_hook` is not supported yet (use the sidecar `authenticate` handler); topics and rooms are not exposed yet.
- Under a sanitizer build the library cannot be loaded into `php` (PHP loads extensions with `RTLD_DEEPBIND`), so the end-to-end test `php_embedded_interop` runs in the release preset only; the cdef drift test runs everywhere.

`src/Embedded/wsrv.cdef` is generated from the C header: `php tools/gen_cdef.php`. A test fails when it is stale.

## Rules that keep it stable

- **Read continuously.** The daemon slows browsers down when you read slowly and, after `ipc_stall_ms` (default 10 s) of you reading nothing, drops your worker and closes its connections with 1001. Keep handlers short; hand slow work to another process.
- **One worker per daemon.** A second application gets `HandshakeRefused` with `StatusCode::BACKPRESSURE`.
- **Restarts.** If your worker dies, the connections it knew are closed with 1012 after the daemon's `shutdown_grace_ms`; browsers should reconnect. Messages in flight at that moment are lost (at-most-once).
- **Secrets.** Pass the shared token through the environment or a mode-0600 file, never `argv` (visible in `ps`). The daemon reads its own copy from `--ipc-token-file`.
- **Fork.** Create the `Client` *after* `pcntl_fork()`; a connection must not be shared between processes.
- **Signals.** The library installs none. Use `pcntl_async_signals` and call `$worker->stop()` (see `examples/echo.php`).
- **64-bit PHP only** (connection ids are `u64` on the wire, held in PHP's signed int).

## Tests

```sh
php tests/run.php                              # unit tests: codec, decoder, client, worker, handshake (no daemon)
php tests/interop.php /path/to/build/wsrvd     # against the real daemon with a raw WebSocket client
```

Both also run under CTest (`ctest -R '^php_'`) and in CI.
