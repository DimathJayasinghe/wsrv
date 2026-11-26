# Using wsrv from Laravel

Real-time rooms for a Laravel app without a Node service, an extension or a paid broker. Two processes besides your normal app:

```
Browser  <--WebSocket-->  wsrvd  <--Unix socket-->  php artisan wsrv:work     (decides who joins what)
                            ^
Controller / job / listener --HTTP POST /publish--+                           (sends)
```

- **`wsrvd`** owns every browser connection (the C++ daemon, or the `ghcr.io/dimathjayasinghe/wsrvd` image). It holds the sockets, so PHP-FPM never does.
- **`wsrv:work`** is a long-running Artisan command (below). It answers "may this browser connect?" and "may this connection join this room?". Nothing else.
- **Your app** publishes with `Wsrv\Publisher`: one short HTTP request per message, from anywhere (a controller, a queued job, a model observer).

**Honest scope.** This is not a Laravel Broadcasting driver: `broadcast(new Event)`, Echo and the Pusher protocol are not implemented. You call `Publisher` yourself. The wsrv calls below are covered by the package's tests (including a run against a real `wsrvd`). The Laravel glue was run by hand on 2026-11-23 against a fresh Laravel 13.43 app with the published `0.1.0-rc.2` image and package: the ticket flow (no ticket, wrong ticket, reused ticket: all 401; a good one: 101), the worker joining only rooms the ticket allows, a `Publisher` resolved from the container delivering to the browser, and the queued listener's retry/fail paths. Since then the worker's own reconnect loop was replaced by `Wsrv\Supervisor` and the browser snippet by `clients/js`: both are covered by this repository's tests (including restarts of a real `wsrvd` and worker under a connected client) but were not run again inside a Laravel app. Two things differ from your app: the test used a stub user in place of the `auth` middleware and your `rooms()` relation, and none of it runs in this repository's CI. Treat it as a tested pattern, not a tested package.

## 1. Install

```
composer require wsrv/client:^1.0@rc     # a plain `composer require wsrv/client` works once 1.0.0 is out
```

Run the daemon (Docker is the easiest; the compose file in `examples/docker` shows the wiring, and `docs/OPERATIONS.md` section 3.1.1 lists every flag):

```
wsrvd --ipc-socket=/run/wsrv/ipc.sock --listen-port=8080 \
      --allow-origin=https://app.example.com \
      --handshake-hook=true \
      --broker=true --publish-port=9200 --publish-token-file=/run/wsrv/publish.token
```

The publish token is a file of at least 32 random bytes, mode `0600`, owned by the daemon's user (for example `openssl rand -hex 32 > /run/wsrv/publish.token`). Your app reads the same file. Never put the token in `.env` of a world-readable path, and never expose port 9200 to the internet: it is for your app servers only.

## 2. Config and the publisher

`config/wsrv.php`:

```php
<?php

return [
    'socket'       => env('WSRV_SOCKET', '/run/wsrv/ipc.sock'),
    'publish_url'  => env('WSRV_PUBLISH_URL', 'http://127.0.0.1:9200'),
    'publish_token_file' => env('WSRV_PUBLISH_TOKEN_FILE', '/run/wsrv/publish.token'),
    'ticket_ttl'   => 30,   // seconds a connection ticket stays valid
];
```

`app/Providers/WsrvServiceProvider.php` (add `App\Providers\WsrvServiceProvider::class` to the list in `bootstrap/providers.php`):

```php
<?php

namespace App\Providers;

use Illuminate\Support\ServiceProvider;
use Wsrv\Publisher;

class WsrvServiceProvider extends ServiceProvider
{
    public function register(): void
    {
        $this->app->singleton(Publisher::class, fn () => new Publisher(
            config('wsrv.publish_url'),
            trim((string) file_get_contents(config('wsrv.publish_token_file'))),
        ));
    }
}
```

## 3. Letting a browser connect: a one-time ticket

The browser cannot send your session cookie to a WebSocket on another port in a way the worker can decrypt (Laravel encrypts it). Use a **ticket**: a random, single-use, short-lived string your app hands to a logged-in user, which the browser passes in the WebSocket URL.

`routes/web.php` (merge the `use` lines with the ones the file already has):

```php
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Cache;
use Illuminate\Support\Facades\Route;
use Illuminate\Support\Str;

Route::middleware('auth')->post('/ws-ticket', function (Request $request) {
    $ticket = Str::random(40);
    Cache::put("wsrv:ticket:$ticket", [
        'user'  => $request->user()->id,
        'rooms' => $request->user()->rooms()->pluck('id')->map(fn ($id) => "room.$id")->all(),
    ], config('wsrv.ticket_ttl'));
    return response()->json(['ticket' => $ticket]);
});
```

The ticket carries the rooms this user may join, decided by your authorization code at the moment of issue. The worker never touches the database.

## 4. The worker

`app/Console/Commands/WsrvWork.php` (Laravel registers commands in `app/Console/Commands` automatically):

```php
<?php

namespace App\Console\Commands;

use Illuminate\Console\Command;
use Illuminate\Support\Facades\Cache;
use Wsrv\Connection;
use Wsrv\Supervisor;
use Wsrv\Upgrade;
use Wsrv\Worker;

class WsrvWork extends Command
{
    protected $signature = 'wsrv:work';
    protected $description = 'Answer wsrvd: who may connect, who may join which room';

    /** @var array<int, array{user:int, rooms:list<string>}> connection id => what its ticket allowed */
    private array $sessions = [];

    public function handle(): int
    {
        // Supervisor reconnects to wsrvd with backoff after a restart and stops on SIGTERM; it calls
        // the closure below with a fresh Worker for every new connection to the daemon.
        $supervisor = Supervisor::forSocket(config('wsrv.socket'));
        $supervisor->installSignalHandlers();
        $supervisor->run(function (Worker $worker): void {
            $this->sessions = [];  // connection ids restart with the daemon: forget the old ones
            $this->configure($worker);
        });
        return self::SUCCESS;
    }

    private function configure(Worker $worker): void
    {
        $worker
            ->on('authenticate', function (Upgrade $u): bool|int {
                parse_str($u->request->query, $query);                 // decodes %xx
                $ticket = $query['ticket'] ?? null;
                if (!is_string($ticket) || strlen($ticket) !== 40) {
                    return 401;
                }
                $grant = Cache::pull("wsrv:ticket:$ticket");           // pull = read and delete: single use
                if (!is_array($grant)) {
                    return 401;
                }
                $this->sessions[$u->conn] = $grant;                    // the question id is the connection id
                return true;
            })
            ->on('message', function (Connection $c, string $data): void {
                $join = json_decode($data, true)['join'] ?? null;
                if (is_string($join) && in_array($join, $this->sessions[$c->id]['rooms'] ?? [], true)) {
                    $c->subscribe($join);
                }                                                       // anything else: ignored
            })
            ->on('close', function (Connection $c): void {
                unset($this->sessions[$c->id]);                         // the daemon drops its rooms itself
            });
    }
}
```

Keep it this small. The handler runs for every browser, one event at a time, so no DB queries and no network calls in it: the ticket lookup is a cache read. `Supervisor` keeps the worker following the daemon across restarts; run the command itself under systemd, Supervisord or a Kubernetes container, and restart it on deploy (`php artisan wsrv:work` does not reload your code by itself). A deploy of the worker closes the browsers' sockets with 1012 after `shutdown_grace_ms`: the reconnecting client in `clients/js` handles that.

## 5. Publishing from your app

From anywhere in Laravel:

```php
use Wsrv\Publisher;

app(Publisher::class)->publish("room.$room->id", json_encode(['text' => $message->body, 'from' => $user->name]));
```

`publish()` returns a `PublishResult` (`subscribers`, `accepted`, `refused`) and throws `Wsrv\PublishFailed` otherwise. In a request, usually do it in a **queued listener** so a busy or restarting daemon does not slow or fail the page, and let the queue retry what is worth retrying:

```php
<?php

namespace App\Listeners;

use App\Events\MessagePosted;
use Illuminate\Contracts\Queue\ShouldQueue;
use Illuminate\Queue\InteractsWithQueue;
use Wsrv\PublishFailed;
use Wsrv\Publisher;

class PushMessageToRoom implements ShouldQueue
{
    use InteractsWithQueue;      // gives the listener $this->fail()

    public int $tries = 5;
    public array $backoff = [1, 5, 15, 60];

    public function handle(MessagePosted $event, Publisher $publisher): void
    {
        try {
            $publisher->publish("room.{$event->message->room_id}", $event->message->toJson());
        } catch (PublishFailed $e) {
            if (!$e->isRetryable()) {
                $this->fail($e);      // wrong token, bad topic, too big: retrying changes nothing
                return;
            }
            throw $e;                  // 429, 503, daemon unreachable: the queue tries again later
        }
    }
}
```

Set `except` to the sender's connection id (the browser can send it in the request) when the sender already shows the message locally: `publish($topic, $payload, except: $connectionId)`.

## 6. The browser

Use the reconnecting client from `clients/js` (copy `src/client.js`, or import it from your bundler): browsers do not reconnect by themselves, and every deploy of `wsrvd` or of the worker closes the sockets.

```js
import { WsrvClient } from './client.js';

const client = new WsrvClient('wss://ws.example.com/', {
  // called before every attempt, because each ticket works once
  ticket: async () => (await (await fetch('/ws-ticket', { method: 'POST', headers: { 'X-CSRF-TOKEN': csrf } })).json()).ticket,
  // this guide's worker reads {"join": "<room>"}; the client's default message is "join <room>"
  joinMessage: (room) => JSON.stringify({ join: room }),
});
client.on('message', ({ data }) => render(JSON.parse(data)));
client.join('room.7');   // remembered, and re-sent after every reconnect
client.connect();
```

Without the client, do the same by hand: open the socket with `?ticket=...`, send the join message in `onopen`, and on `onclose` wait (backoff, jitter), fetch a **new** ticket and start over.

The `Origin` check is the daemon's `--allow-origin`: set it to your site, not `*`.

## 7. Things to know before production

- **TLS.** `wsrvd` can terminate TLS itself, or sit behind your proxy (Nginx, Caddy, a load balancer) that does. `Publisher` speaks plain `http://`: keep port 9200 on a private network or a loopback.
- **The ticket is in a URL**, so it can appear in proxy logs. It is single-use and expires in `ticket_ttl` seconds, which makes a logged one worthless. Keep the TTL short.
- **Delivery is at most once.** A publish to a subscriber whose queue is full is refused (`refused` in the result), and a browser that is offline misses the message. Store messages in your database and let a reconnecting client fetch what it missed; the room is for "now".
- **One worker at a time.** The daemon talks to a single application connection. Run one `wsrv:work`; your process manager restarts it if it dies, and the daemon refuses browsers (503) in between rather than accepting unauthenticated ones.
- **Limits are real.** Topic names are `[A-Za-z0-9._:/@-]` up to 128 bytes, and the broker bounds topics, subscriptions and payload size (`docs/OPERATIONS.md`). `LIMIT` on a subscribe means a bound was hit, not that retrying soon will help.
