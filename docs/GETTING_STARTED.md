# Getting started with wsrv

wsrv is a WebSocket server for PHP. A small daemon, `wsrvd`, holds the connections; your PHP code is a normal worker that reacts to events (a browser connected, a message arrived, a connection closed) and sends replies. PHP never holds a socket open, so a slow handler cannot block the others.

```
browser ──WebSocket──▶ wsrvd ──Unix socket (or TCP, 1.1)──▶ your PHP worker
                         ▲
controller / queue job ──HTTP POST /publish (token)
```

Pick the path that matches what you have. Every path ends with something you can see working. This guide describes wsrv 1.1; the quickstart uses the 1.1 image (the compose default; it needs 1.1 because it makes the token with `--generate-token`), section 5 needs the 1.1 image and `wsrv/client` 1.1, and everything else also works on 1.0.

| You have | Go to | You end with | Time |
|---|---|---|---|
| Docker and five minutes | [1. See it work](#1-see-it-work-docker-five-minutes) | a browser page talking to the daemon, a PHP worker answering, and a reconnect you can watch | 5 min |
| A PHP project on Linux or in a container | [2. Add it to a PHP project](#2-add-it-to-a-php-project) | your own worker answering a browser | 15 min |
| A Laravel app | [3. Laravel](#3-laravel) | ticket login, an Artisan worker, queued publishing | 30 min |
| A controller, queue job or cron script that must push to browsers | [4. Publish from anywhere](#4-publish-from-anywhere) | one HTTP call that reaches everyone in a room | 10 min |
| A Mac or a Windows PC | [5. macOS and Windows](#5-macos-and-windows) | your worker on the host, the daemon in Docker | 10 min |
| A web page that must survive deploys | [6. The browser client](#6-the-browser-client) | a client that reconnects and rejoins its rooms | 5 min |
| Something other than PHP | [7. Another language](#7-another-language) | the protocol and the C ABI | - |

## 1. See it work (Docker, five minutes)

You need Docker with Compose. No compiler, no PHP on your machine.

```sh
git clone https://github.com/DimathJayasinghe/wsrv && cd wsrv/examples/docker
docker compose up -d
```

Open <http://localhost:8000>, press **Connect**, then **Send**. The page sends `hello`, the PHP worker answers `hello`, and the log shows both. Then:

1. **Join a room.** Press **Join** (the page uses `room.1`).
2. **Publish from a separate process,** the way a controller or a queue job would:

   ```sh
   docker compose run --rm worker php /app/publish.php room.1 "hello from outside"
   # subscribers=1 accepted=1 refused=0     and the page shows: hello from outside
   ```
3. **Restart the daemon** and watch the page recover by itself:

   ```sh
   docker compose restart wsrvd
   ```

   The log shows `closed: code 1001 (server shutting down)`, a retry after a fraction of a second, `reconnected`, and `joined room.1` again: the client remembered the room.

Ports 8080 (the daemon) or 8000 (the page) busy? `WSRV_PORT=18080 WSRV_WEB_PORT=18000 docker compose up -d`, then type `ws://localhost:18080/` into the page. Stop everything with `docker compose down -v`.

What you just ran, in `examples/docker`: `wsrvd` (the daemon image), a 27-line PHP worker (`worker/worker.php`), a 21-line publisher (`worker/publish.php`) and the page (`web/index.html`, served by PHP's built-in server on your loopback: it is a demo server, not part of wsrv). The daemon only accepts browsers from `http://localhost:8000`; `wscat` and `websocat` send no `Origin` and are let in by `WSRVD_ALLOW_MISSING_ORIGIN`, which you remove for anything real. The details: [examples/docker/README.md](../examples/docker/README.md).

## 2. Add it to a PHP project

You need PHP 8.2 or newer and Composer. The PHP package has no extension requirement for the sidecar.

**1. Get the daemon.** Either the image (`ghcr.io/dimathjayasinghe/wsrvd`, also on Docker Hub as `dimathjayasinghe/wsrvd`; amd64 and arm64) or the checksummed Linux binary from the [releases](https://github.com/DimathJayasinghe/wsrv/releases). Run it beside your PHP code. With the binary:

```sh
mkdir -m 700 /tmp/wsrv                       # the socket directory is private and yours
wsrvd --ipc-socket=/tmp/wsrv/ipc.sock --listen-port=8080 --allow-origin=http://localhost:8000
```

With Docker, the worker must share the socket, so run it in a container beside the daemon: the compose file in `examples/docker` is the pattern to copy (both containers run as uid 10001, because the daemon refuses a peer with another uid).

**2. Install the package and write the worker.**

```sh
composer require wsrv/client
```

```php
<?php
// worker.php: the whole PHP side
require __DIR__ . '/vendor/autoload.php';

use Wsrv\{Connection, Supervisor, Worker};

// Supervisor is the loop a production worker needs: it reconnects to the daemon after a restart, and a
// handler that throws is logged instead of ending the worker.
Supervisor::forSocket('/tmp/wsrv/ipc.sock')->run(function (Worker $worker): void {
    $worker->on('message', fn (Connection $c, string $data, bool $binary) =>
        $binary ? $c->sendBinary($data) : $c->send($data));
});
```

```sh
php worker.php
```

**3. Connect from a browser** served from the origin you allowed (or from a terminal, see below):

```js
const ws = new WebSocket('ws://localhost:8080/');
ws.onmessage = (e) => console.log(e.data);
ws.onopen = () => ws.send('hello');   // the worker sends it back
```

A command-line client such as `websocat` sends no `Origin` header and is refused with `403` unless the daemon was started with `--allow-missing-origin=true`. That is fine for a local experiment; think before using it on anything a browser can reach.

The full API (events, authentication hook, rooms, supervisor): [bindings/php/README.md](../bindings/php/README.md). Going to production: [section 8](#8-before-you-go-to-production).

## 3. Laravel

[docs/LARAVEL.md](LARAVEL.md) is a complete guide: install, config, a **one-time ticket** so only logged-in users can connect, an Artisan worker that decides who may join which room, publishing from controllers and queued jobs, and the browser side.

**Two ways.** The guide above is for your own rooms and join messages: you call `Wsrv\Publisher` yourself. If you want `broadcast(new Event)`, `Broadcast::channel()` and Laravel Echo, use [docs/LARAVEL_ECHO.md](LARAVEL_ECHO.md) (the `wsrv` broadcasting driver and `php artisan wsrv:echo`, planned for 1.2); one daemon serves one of the two at a time.

**Honest scope of the ticket guide.** The wsrv calls in the guide are covered by the package's tests, including a run against a real daemon; the Laravel glue was tried by hand against Laravel 13 and is not in CI.

## 4. Publish from anywhere

A PHP-FPM request cannot hold a WebSocket, but it can make one HTTP call. Start the daemon with the broker and the publish endpoint (the token is a file the daemon makes itself; since 1.1 it accepts no other):

```sh
sudo install -d -m 700 -o "$USER" /run/wsrv      # a private directory for the socket and the token (or use one you own)
wsrvd --generate-token=/run/wsrv/publish.token
wsrvd --ipc-socket=/run/wsrv/ipc.sock --broker=true \
      --publish-port=9200 --publish-token-file=/run/wsrv/publish.token
```

The token file is mode 0600 and owned by the user who generated it, so generate it as the user your PHP processes run as, or copy it for them with `install -m 0600 -o <user>`. The worker decides who joins which room (it has the session); the publisher only sends:

```php
// in the worker: the browser asks, your code decides
$worker->on('message', function (Connection $c, string $data) {
    if (userMayJoin($c, $data)) { $c->subscribe($data); }      // e.g. "room.7"
});

// anywhere else (a controller, a queued job, cron): one short HTTP call
$publisher = new Wsrv\Publisher('http://127.0.0.1:9200', trim(file_get_contents('/run/wsrv/publish.token')));
$result = $publisher->publish('room.7', json_encode(['text' => 'hello']));
// $result->subscribers, ->accepted, ->refused
```

The publish endpoint is plain HTTP: keep it on loopback or a private network (the compose example never publishes it to the host). Several daemons? `ClusterPublisher` sends to each of them; there is no built-in message backplane between nodes yet ([DEPLOYMENT.md section 7](DEPLOYMENT.md)).

## 5. macOS and Windows

The daemon runs on Linux only (it is built on epoll); on a Mac or a PC you run it in Docker Desktop or WSL2. Since 1.1 your PHP worker can still run natively on the host, because the link between the two can be TCP:

```sh
cd examples/host-worker
docker compose up -d
export WSRV_TOKEN="$(docker compose run --rm --no-deps --entrypoint cat token-init /run/wsrv/ipc.token)"
composer require "wsrv/client:^1.1"
php worker.php
```

Connect a WebSocket client to `ws://localhost:8080/`. The link is **not encrypted** (the compose file publishes it on your loopback only), so this is for development; in production the worker sits beside the daemon on Linux. What is and is not tested on those platforms: [DEPLOYMENT.md section 11](DEPLOYMENT.md#11-macos-and-windows).

## 6. The browser client

A bare `WebSocket` does not reconnect when you deploy or restart the daemon. `clients/js` is one dependency-free file (browser and Node 22+) that reconnects with jittered backoff, asks for a fresh ticket on every attempt, and joins its rooms again:

```js
import { WsrvClient } from './client.js';       // copy clients/js/src/client.js into your project

const client = new WsrvClient('wss://app.example.com/ws', {
  ticket: async () => (await fetch('/ws-ticket', { method: 'POST' }).then((r) => r.json())).ticket,
});
client.on('message', ({ data }) => render(JSON.parse(data)));
client.on('reconnecting', ({ attempt, delay }) => showBanner(`reconnecting (${attempt})...`));
client.join('room.7');       // remembered, sent again after every reconnect
client.connect();
```

It is not on npm yet, and it is outside the 1.x stability promise. Options, events and limits (no message replay: delivery is at most once): [clients/js/README.md](../clients/js/README.md). `examples/docker/web/index.html` is a complete page that uses it.

## 7. Another language

`wsrvd` speaks a small, documented binary protocol over a Unix socket or TCP ([docs/IPC_PROTOCOL.md](IPC_PROTOCOL.md)), and the engine is also a C library with a stable ABI ([docs/BINDINGS.md](BINDINGS.md), `examples/echo_c`). PHP has a worker client (`wsrv/client`); a Node or Python worker client does not exist yet. The JavaScript client is for the browser side (it speaks WebSocket), not for writing a worker. A client is mostly framing, and the protocol document lists every frame and the rules a client must keep.

## 8. Before you go to production

The checklist is [DEPLOYMENT.md section 9](DEPLOYMENT.md). The six things people most often miss:

- **Origins.** Set `allow_origin` to your sites, and remove `allow_missing_origin` unless only non-browser clients connect.
- **Behind a reverse proxy,** set `trusted_proxy` to the proxy so per-IP limits and logs see real clients. Without it every user looks like one address ([OPERATIONS.md](OPERATIONS.md) section 1.2.1).
- **TLS:** terminate it at the proxy (and give the daemon `trusted_proxy`) or give the daemon `tls_cert` and `tls_key`.
- **Memory limit.** Set `max_buffered_bytes` to about half of the memory you give the process; the default (1 GiB) is for hosts with several GiB.
- **Metrics and health:** `admin_port` serves `/metrics`, `/healthz`, `/readyz`. Keep it on loopback or behind a network policy, and set `admin_token_file` if a scraper reaches it over a network.
- **Logs:** one structured line per event on stderr (in the quickstart: `docker compose logs wsrvd`); the client address appears only with `log_peer`.

## If it does not work

| You see | Because | Do |
|---|---|---|
| Browser: the connection fails, daemon log says the origin was refused | the page's origin is not in `allow_origin` | add the exact origin (`https://app.example.com`, with scheme and port) |
| `403` from `wscat` or `websocat` | no `Origin` header | `--allow-missing-origin=true` for local tests only |
| The worker cannot connect, or the daemon logs a refused peer | the socket directory or the worker's user is wrong: the socket is mode 0600 and the daemon checks the peer uid | run the worker as the same user (uid 10001 in the containers) |
| `wsrvd` refuses to start and says the token must be made by `--generate-token` | since 1.1 the publish token (and the IPC token over TCP) must be generated | `wsrvd --generate-token=FILE` and give the file to your publishers |
| A new client gets `429` (or `503` with `Retry-After`) | a per-address limit, or that address table is full (`ip_limit` in the log) | raise `max_connections_per_ip`, or set `trusted_proxy` if a proxy hides your users |
| A new client gets `503` with `Retry-After: 1` | the server holds more than `max_buffered_bytes` (since 1.1; `buffer_budget` in the log) | raise the limit together with `max_message_size`, or add capacity |
| A new client is closed with no answer at all | `max_connections` is reached (`conn_limit` in the log) | raise it, or add capacity |
| Everything works from your laptop but users share one connection allowance | a proxy hides the clients | set `trusted_proxy` |

Questions and problems: [open an issue](https://github.com/DimathJayasinghe/wsrv/issues). Security reports: [SECURITY.md](../SECURITY.md).
