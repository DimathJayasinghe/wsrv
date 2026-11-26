# wsrv/client: PHP client for the `wsrvd` WebSocket sidecar

A pure-PHP client (PHP 8.2+, plain streams, no extensions needed) for the **wsrvd** daemon. The daemon owns the WebSocket connections, framing, UTF-8 checks, limits and backpressure; your PHP code only receives events and sends commands over a local Unix socket. That is what makes it work on every PHP stack, including PHP-FPM workers and Laravel/Symfony queue workers: PHP never holds a WebSocket or a thread.

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
| `Connection::subscribe($topic)` / `unsubscribe($topic)`, `Worker::publish($topic, $payload, $binary = false, $except = 0)` | rooms over IPC (daemon with `--broker=true`) |
| `new Publisher($url, $token)` then `->publish($topic, $payload, $binary = false, $except = 0)` | publish from any process over HTTP; returns `PublishResult`, throws `PublishFailed` |
| `new ClusterPublisher([$url1, $url2], $token)` then the same `->publish(...)` | publish to every daemon of a multi-node deployment; returns `ClusterPublishResult` (summed counts, `->failures` per node), throws `PublishFailed` only when no node took it |
| `Supervisor::forSocket($socket, $token)->run(fn (Worker $w) => ...)` | the production loop: reconnects to the daemon with backoff and jitter, retries `BACKPRESSURE`, fails fast on a wrong token, reports throwing handlers without dropping the link, stops on `SIGTERM` (`installSignalHandlers()`) |
| `Upgrade::accept($subprotocol = null)` / `reject($status = 403)` | answer a waiting upgrade (`authenticate`) |
| `HandshakeRequest::cookie($name)` / `header($name)` / `headers($name)` | read the request in `authenticate` |

A command's request id comes back later in a `StatusReply` (`'status'` handler). `StatusCode::OK` means *accepted*, not delivered. Ids number from 1; id 0 is reserved.

## Authenticating connections

Turn the hook on in the daemon (`--handshake-hook=true --handshake-forward-header=Cookie`) and register an `authenticate` handler. No browser gets a WebSocket until it answers:

```php
$worker->on('authenticate', fn (Upgrade $u) => $u->request->cookie('session') === lookup_session());
```

Return `true` (accept), `false` (403), an int status (`400, 401, 403, 404, 429, 500, 503`), a string (accept, choosing that subprotocol), or `null` to answer later with `$u->accept()` / `$u->reject()`. **It fails closed:** no handler, a handler that throws, an unusable return value, or no answer within the daemon's `handshake_decision_timeout_ms` (default 3 s) all refuse the browser (500 or 503), never accept. The daemon also refuses at once while your worker is not connected. Everything the request carries (`$u->request`) comes from the browser: validate it. See `examples/authenticate.php` and `docs/OPERATIONS.md`.

## Rooms, and publishing from a controller or a job

Start the daemon with the broker and the publish endpoint (the token is a file, mode 0600, at least 32 bytes):

```
wsrvd --ipc-socket=/run/wsrv/ipc.sock --broker=true --publish-port=9200 --publish-token-file=/run/wsrv/publish.token
```

The **worker** decides who joins which room (it has the session), the **publisher** sends:

```php
// worker (long-running CLI process): the browser asks, your code decides, then
$worker->on('message', function (Connection $c, string $data) {
    if (userMayJoin($c, $data)) { $c->subscribe($data); }   // e.g. "room.7"
});

// anywhere else (PHP-FPM controller, queued job, cron): one short HTTP call
$publisher = new Publisher('http://127.0.0.1:9200', trim(file_get_contents('/run/wsrv/publish.token')));
$result = $publisher->publish('room.7', json_encode(['text' => 'hello']), except: $senderConnectionId);
// $result->subscribers, ->accepted, ->refused
```

`publish()` throws `PublishFailed` on anything but a 200; `$e->status` is the HTTP status and `$e->isRetryable()` is true for 429, 503 and an unreachable daemon. A subscription ends with its connection, and also at once if the worker disconnects. Only `http://` is supported (the token travels in clear text: keep the daemon on a private network or behind a TLS proxy). Endpoint reference and bounds: `docs/OPERATIONS.md` 3.1.1.

Using Laravel? `docs/LARAVEL.md` has the ticket login, an Artisan worker and a queued publisher.

## Quick start: embedded mode (about five minutes)

No daemon, but you need `libwsrv.so` and PHP with `ext-ffi` (the CLI default allows it).

**Get the library.** On glibc Linux (x86_64 or aarch64, glibc 2.39 or newer, OpenSSL 3) a release has a prebuilt one (on macOS and Windows there is none: the server is Linux only, so run the Docker image and your worker beside it, see docs/DEPLOYMENT.md section 11; the sidecar client itself works there). Install it on purpose, with a checksum check (Composer never downloads it for you, ADR-0026):

```sh
composer require wsrv/client                     # the PHP code
vendor/bin/wsrv-install-lib --dir=wsrv-lib        # downloads libwsrv.so of this package's version, checks SHA-256
export WSRV_LIBRARY=$PWD/wsrv-lib/libwsrv.so      # the command prints this line
```

Add `--daemon` to get `wsrvd` too, `--sha256=HASH` to pin a hash you trust (for example after `gh attestation verify wsrv-lib/libwsrv.so --repo OWNER/wsrv`), or `--from-dir=DIR` to install from files you downloaded yourself (offline). Anything else (musl, macOS, older glibc), or no release yet: build it once (any C++23 toolchain, or the dev container):

```sh
cmake --preset release && cmake --build --preset release --target wsrv_shared   # build/release/src/capi/libwsrv.so
export WSRV_LIBRARY=$PWD/build/release/src/capi/libwsrv.so
php bindings/php/examples/embedded_echo.php --port 8080 --allow-missing-origin     # echo server
php bindings/php/examples/chat/main.php --port 9002 --allow-missing-origin         # chat rooms
```

Connect a WebSocket client to `ws://127.0.0.1:8080/` (the echo) or `ws://127.0.0.1:9002/` (send `join lobby`, then `say lobby hello`). `--allow-missing-origin` is for command-line clients, which send no `Origin`; a browser page needs `--allow-origin` instead. The chat is `examples/chat/Chat.php`, about 150 lines, in the text protocol of the C++ `examples/chat`; PHP has no broker yet, so it keeps the rooms itself and owns every bound (lesson 39).

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
- It refuses PHP-FPM and any SAPI but `cli`/`phpdbg`. Create the server after `fork()`. Topics and rooms are not exposed yet.
- **Authenticating** works as in the sidecar: set `'handshake_hook' => true` (and `'handshake_forward_header' => ['Cookie']`), register `on('authenticate', fn (Wsrv\Embedded\Upgrade $u) => $u->request->cookie('session') === lookup_session())`. Same return values; it fails closed (a throwing handler or an unusable value refuses with 500), and `start()` throws if the setting and the handler do not agree. See ADR-0025 A1.
- **Logs**: the library's threads write one structured line per event (`event=listening`, `origin_refused`, `slow_consumer`...) to the process's stderr, never calling into PHP. `'log_level' => 'warn'` quiets it (`error`, `warn`, `info` default, `debug`); `'log_peer' => true` adds client addresses (personal data, off by default). The events are listed in `docs/OPERATIONS.md` section 2.
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
