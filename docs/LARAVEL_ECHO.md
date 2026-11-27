# Laravel Echo on wsrv

Use Laravel's own broadcasting (`broadcast(new Event)`, `Broadcast::channel()`, `routes/channels.php`, Laravel Echo in the browser) with `wsrvd` as the WebSocket server, so a Laravel app gets real-time events without a Node service, a paid broker or a second WebSocket server to learn.

> **Status.** Planned for wsrv 1.2 (ADR-0042). The gateway, the broadcaster, encrypted channels and `php artisan wsrv:echo` are implemented and tested: unit tests, a run against a real `wsrvd` with raw WebSocket clients, a run of the glue inside a real Laravel 13 application, and two runs of the real browser libraries (`pusher-js` 8.6.0 and `laravel-echo` 2.6.3, in Chrome): against a real `wsrvd`, and against a real Laravel 13 application with a real `wsrvd`. Section 12 says what each run covers and what was not run.

If you want rooms without Echo (your own join messages, your own tickets), see [LARAVEL.md](LARAVEL.md). The two ways use the same daemon, but one daemon serves one of them at a time (one worker per daemon).

## 1. How it fits together

```
Browser (Echo / pusher-js)  <--WebSocket-->  wsrvd  <--Unix socket-->  php artisan wsrv:echo
                                               ^                         (the gateway: who may join what)
Laravel: broadcast(new Event) --HTTP POST /publish--+
Laravel: POST /broadcasting/auth  (your channel callbacks decide; the answer is a signature)
```

- The **browser** speaks the Pusher WebSocket protocol, as Echo and `pusher-js` do.
- **`wsrvd`** holds the sockets and does the fan-out. It stays protocol-agnostic.
- **`php artisan wsrv:echo`** is the *gateway*: it turns Pusher frames into joins and leaves, verifies the signatures your `/broadcasting/auth` route gave out, keeps the member list of presence channels and relays whispers. It never sees your broadcasts: they go from Laravel straight to the daemon.
- **Laravel** uses a broadcasting driver named `wsrv` (`Wsrv\Laravel\WsrvBroadcaster`), which signs private and presence channels and publishes events over the daemon's HTTP publish endpoint.

The stock `pusher` and `reverb` drivers do **not** work against wsrv: they call Pusher's HTTP API, which wsrv does not implement. Use the `wsrv` driver.

## 2. Install

```
composer require wsrv/client
```

Laravel 11 or newer. The package registers itself through package discovery. (Without discovery, add `Wsrv\Laravel\WsrvServiceProvider::class` to `bootstrap/providers.php`.) Encrypted channels also need `ext-sodium`, which ships with PHP.

Run the daemon. Compared with [LARAVEL.md](LARAVEL.md) there is no handshake hook, a Pusher frame is small, and the daemon's rooms and its frame to the worker must be sized for a channel per user:

```
wsrvd --ipc-socket=/run/wsrv/ipc.sock --listen-port=8080 \
      --allow-origin=https://app.example.com \
      --broker=true --publish-port=9200 --publish-token-file=/run/wsrv/publish.token \
      --max-message-size=16384 --ipc-max-frame=524288 \
      --broker-max-topics=50000 --broker-max-subscriptions=500000
```

Make the publish token with `wsrvd --generate-token=/run/wsrv/publish.token`, as in [LARAVEL.md](LARAVEL.md). Every setting is also an environment variable (`WSRVD_BROKER_MAX_TOPICS=50000`, ...). The numbers on the last two lines are sized for a site with up to about 10 000 browsers online; the daemon's defaults are small for Echo:

| Daemon setting | Default | What Echo needs |
|---|---|---|
| `max_message_size` | 1 MiB | about the gateway's `max_frame_bytes` (16384): a Pusher frame is small |
| `broker_max_topics` | 1 024 | the channels that have a browser in them at the same time: one per user for per-user private channels, plus rooms, plus public channels. A site with a private channel per user passes 1 024 at about a thousand users online. Range 1 to 1 000 000 |
| `broker_max_subscriptions` | 100 000 | browsers times channels each, at the same time (20 000 browsers with 4 channels each is 80 000). Range 1 to 10 000 000 |
| `broker_max_topics_per_connection` | 64 | at least the gateway's `max_channels_per_connection` (64), or the daemon refuses joins the gateway let through. Range 1 to 1 024 |
| `ipc_max_frame` | `max_message_size` + 64 KiB | the largest presence member list you will send (below). Range 4 096 to 67 108 864, and at least `max_message_size` + 18 |

When a bound is too small the browser gets `pusher:subscription_error` (`LimitReached`) for that join, and the statistics line counts it in `daemon_refused` (section 10). With 3 000 browsers and the default 1 024 topics, 3 918 of 12 000 joins were refused that way in the load run (section 12), and nothing was left behind. Do not simply set the bounds to their ceilings: any browser may name any public channel, each distinct name is a room until its last member leaves, and these bounds are what stops a script from filling the daemon with junk ones. Size them to your real load with some headroom, and watch `daemon_refused`. The daemon also limits each client address to 100 live connections and 50 new ones a second ([OPERATIONS.md](OPERATIONS.md) section 1.2); behind a proxy, set `trusted_proxy` (section 9).

**A presence member list must fit one frame.** A presence join is answered with the whole member list, which is about `members x (the JSON size of user_info + 40 bytes)`: 100 members with 300-byte `user_info` is 33 KiB, with 1 000-byte `user_info` 101 KiB, and 1 000 members with 300 bytes 329 KiB. Text made mostly of quotation marks or backslashes takes up to twice its signed size (the list is JSON inside JSON), so the worst case with the defaults (100 members, 2 048 bytes of signed data each) is a little over 400 KB, and `--ipc-max-frame=524288` makes every possible list fit. A list that does not fit the frame fails **that one join** with `pusher:subscription_error` ("The member list is too large to send") and nothing else: the room, the other members and the link to the daemon are untouched (checked against a real daemon, section 12). Raise `ipc_max_frame`, or lower `max_presence_members` or what your channel callback returns as `user_info`.

## 3. Configure Laravel

`.env`:

```
BROADCAST_CONNECTION=wsrv
WSRV_APP_KEY=any-public-name        # what Echo is given as `key`
WSRV_APP_SECRET=at-least-16-random-bytes-never-in-the-browser
WSRV_PUBLISH_TOKEN_FILE=/run/wsrv/publish.token
```

`config/broadcasting.php`, in `connections`:

```php
'wsrv' => [
    'driver' => 'wsrv',
    'key' => env('WSRV_APP_KEY'),
    'secret' => env('WSRV_APP_SECRET'),
    'publish_url' => env('WSRV_PUBLISH_URL', 'http://127.0.0.1:9200'),
    'publish_token_file' => env('WSRV_PUBLISH_TOKEN_FILE'),
    'ipc_endpoint' => env('WSRV_IPC', '/run/wsrv/ipc.sock'),   // or tcp://host:port, then also:
    // 'ipc_token_file' => env('WSRV_IPC_TOKEN_FILE'),
    // 'encryption_master_key_base64' => env('WSRV_ENCRYPTION_KEY'),   // only for private-encrypted-* channels
    // 'options' => ['max_presence_members' => 100, 'client_events' => true],   // see section 7
],
```

The configuration is checked when it is read: a missing key, a secret under 16 bytes, an unknown key or option name (a typo does not silently fall back to a default), or an unreadable token file is an error that names the setting and never prints a secret. The whole connection is needed by every command that boots your routes, `wsrv:echo` included: `routes/channels.php` builds the broadcaster, which needs the publish token, so a worker with no `WSRV_PUBLISH_TOKEN_FILE` stops at start.

Then enable the broadcasting routes as usual (`php artisan install:broadcasting`, or `Broadcast::routes()` in your application) and define channels in `routes/channels.php` exactly as you would for Pusher.

## 4. Run the gateway

```
php artisan wsrv:echo
```

Run one per daemon, under systemd, Supervisor or a container restart policy. It reconnects by itself when the daemon restarts. When the daemon goes away it closes every browser; Echo reconnects and resubscribes, and the new gateway starts with an empty member list, so no presence state outlives the connections it describes. Section 9 has a systemd unit, the proxy, Docker and what a restart does; section 10 the line of statistics it prints every 60 seconds (`--stats-interval=0` turns it off).

## 5. The browser

```js
import Echo from 'laravel-echo';
import Pusher from 'pusher-js';
window.Pusher = Pusher;

window.Echo = new Echo({
    broadcaster: 'reverb',               // the same client settings as for Reverb
    key: 'any-public-name',              // WSRV_APP_KEY
    wsHost: 'ws.example.com',
    wsPort: 8080,
    wssPort: 443,
    forceTLS: true,                      // false for plain ws:// during development
    enabledTransports: ['ws', 'wss'],
});

Echo.channel('orders').listen('OrderShipped', (e) => console.log(e.order));
Echo.private('chat.7').listenForWhisper('typing', (e) => console.log(e.name));
Echo.join('room.1').here(users => {}).joining(user => {}).leaving(user => {});
```

Checked with `broadcaster: 'reverb'` (the options above); the `'pusher'` broadcaster reaches the same `pusher-js` code but was not run. In a Laravel app Echo adds the CSRF header itself, and `/broadcasting/auth` is Laravel's route.

The browser's `Origin` must be in `--allow-origin`. Channel names are limited to letters, digits and `. _ : / @ -` (up to 128 bytes); a name with `=`, `,` or `;` is refused with an error, never rewritten.

## 6. What works, and what does not

| Feature | In wsrv 1.2 |
|---|---|
| Public channels, `broadcast()`, `broadcastAs()`, `broadcastWith()` | yes |
| Private channels and `Broadcast::channel()` callbacks | yes: the signature is bound to the browser's socket id and the channel |
| Presence channels: `here`, `joining`, `leaving` | yes, per daemon; a user with two tabs is one member until the last tab leaves |
| `toOthers()` | yes, through the sender's socket id |
| Whispers (`.whisper()`, `listenForWhisper()`) | yes, only between members of a private or presence channel, rate limited; not on encrypted channels |
| Encrypted channels (`private-encrypted-*`) | yes, with `ext-sodium` and a master key; without a key, broadcasting to one is an error and never sends plaintext |
| Pusher's HTTP API (`pusher/pusher-php-server` pointed at wsrv, `/apps/.../events`) | no |
| User authentication and watchlist (`pusher:signin`, `Echo.user()`) | no: answered with a 4200 error, once, without a strike |
| More than one daemon with the Pusher protocol | no: presence and `toOthers()` are per daemon |
| Delivery to a browser that is offline | no: delivery is at most once; fetch what was missed from your database |

## 7. Limits and options

Every bound has a default for a public site, and each is range-checked. They are set under `'options'`:

| Option | Default | Meaning |
|---|---|---|
| `activity_timeout` | 30 | seconds of silence before the browser pings |
| `max_frame_bytes` | 16384 | a larger frame from a browser is a bad frame |
| `max_channels_per_connection` | 64 | joins on one connection, counting those still waiting for the daemon's answer; the daemon's own `broker_max_topics_per_connection` (config file, default 64) must be at least this |
| `max_presence_members` | 100 | users in one presence channel (a user with two tabs counts once) |
| `max_channel_data_bytes` | 2048 | the signed member data of a presence member |
| `client_events` | true | allow whispers |
| `client_events_per_second` | 10 | whispers per connection per second; one over the rate is answered with `4301` and costs no strike |
| `messages_per_second` | 30 | all frames from one connection, sustained (see "Why a burst" below) |
| `message_burst` | `max_channels_per_connection` + 64 (128) | frames one connection may send at once; the configuration refuses less than `max_channels_per_connection` + 16. The daemon's own `max_data_frames_per_second` (default 10 000, then it closes the connection with 1008) is the outer wall: keep the burst well below it |
| `max_strikes` | 8 | bad frames, and frames over the message rate, before the connection is closed with 1008. A strike is forgiven for each full 60 seconds since the previous one |

**Why a burst.** `pusher-js` sends the subscribe for every channel of a page back to back, on load and again after every reconnect, before the daemon has answered a single one. The limit on all frames is therefore a *token bucket*: it holds `message_burst` frames and gets `messages_per_second` of them back every second, so a page with all of its 64 channels connects without an error, while a connection that keeps sending faster than the sustained rate is answered with `4301` and, after `max_strikes` of those, closed with 1008 (`pusher-js` reconnects by itself, after about a second). A whisper over `client_events_per_second` is refused with `4301` and no strike: a typing indicator that your application does not throttle must not cost a user the connection. But every frame, a whisper too, takes a token from the bucket, so *any* traffic above `messages_per_second` that goes on until the burst is used up ends in a close: at 50 frames a second with the defaults that is after about 7 seconds (128 tokens, 20 a second more than the refill, then 8 strikes in under half a second). A typing indicator never gets near that; an application that shares cursor positions should raise `messages_per_second` and `client_events_per_second` together. Reverb with rate limiting configured answers a client over its rate with the error alone and closes it only if `terminate_on_limit` is set; to make wsrv only refuse, raise `max_strikes` (up to 1 000 000). Strikes are not for life: each full 60 seconds without a new strike forgives one earlier strike (whole strikes, whole minutes), so a tab that stays open for a week and sends one malformed frame a day is never closed for it, while eight within moments are.

A frame that is not valid JSON, has no event, carries a channel name wsrv cannot hold, or holds a number JSON cannot carry back out (such as `1e999`) is answered with Pusher's `4200` error and counts as a strike. A signature that does not verify is answered with `4009`.

## 8. Security notes

- **The app key is public**; it is in your JavaScript. The **secret** never leaves the server, is hidden from `print_r`, `var_dump` and `serialize`, and is used only to sign and to verify.
- A private channel signature covers the browser's socket id and the channel name. Replaying it from another connection fails, and the daemon, not the gateway, is the authority on which connection a socket id belongs to.
- A presence member's user id and info are **inside the signed data**. A browser cannot join as someone else, and a whisper carries the signed user id, never one the browser sent.
- Signatures are compared with `hash_equals`.
- `check_app_key => true` (with the daemon's `--handshake-hook=true`) refuses a URL that does not end in `/app/<key>`; it is off by default, because the key is public and the signature is what protects a channel.
- The publish endpoint (port 9200) is for your app servers only: never expose it to the internet.

## 9. Deploying

**Start order does not matter, unless the daemon runs the handshake hook.** A browser that connects before `wsrv:echo` is running gets an open socket and no frame; when the worker attaches, the browser receives `pusher:connection_established` at once (checked against a real daemon). A worker that is down delays browsers, it does not refuse them. `check_app_key => true` needs the daemon's `--handshake-hook=true`, and with the hook there is nobody to ask while no worker is attached: the daemon refuses the upgrade with `503` at once and logs `handshake_undecided` (checked against a real daemon). In that setup start the worker first, and expect failed connections while it is down.

**One daemon and one worker per application.** There is no backplane between daemons, so a second daemon would hold only the browsers connected to it and get only the broadcasts sent to it. A second `wsrv:echo` against the same daemon is refused: it prints `wsrvd still has the previous application connected, retrying` (about once a second) until the first one is gone, then connects within about a second (checked against a real daemon, Unix socket). High availability here is a restart policy (systemd, a container restart policy), not a second copy.

**A worker that leaves closes its browsers.** When `wsrv:echo` stops (a deploy, a crash) the daemon closes the browser connections it knew with 1012 after `shutdown_grace_ms` (10 seconds by default), and Echo reconnects and joins its channels again. A new worker that attaches during those 10 seconds knows nothing of those browsers (it is told of no connection it did not see open, and what they send is ignored), so a restart takes up to `shutdown_grace_ms` before they are back. That key sets the time (it also bounds how long a graceful daemon stop waits for browsers to answer the close). Plan a worker deploy as a reconnect storm. The daemon and the gateway take it in stride (section 12: 5 000 browsers with four channels each were all answered within 2 seconds), but every private and presence join also calls your `/broadcasting/auth` route again, so your web workers see that burst too.

**The configuration is read once.** The worker reads the connection (key, secret, token files, options) and your application when it starts. After a changed `.env`, a rotated `WSRV_APP_SECRET`, a `config:cache` or a deploy, restart it in the same step that restarts your web servers: until then it verifies with the old secret and every private and presence join fails with `4009`.

### systemd

[`deploy/systemd/wsrv-echo.service`](../deploy/systemd/wsrv-echo.service) runs `php artisan wsrv:echo` next to [`wsrvd.service`](../deploy/systemd/wsrvd.service). It `Wants=` the daemon rather than `Requires=` it (the worker reconnects by itself, so their lifecycles stay apart), uses `Restart=on-failure` with a start limit (a worker that cannot start ends as *failed*, where you will see it, instead of restarting for ever), stops on SIGTERM, and has the daemon unit's hardening with the two directories Laravel writes. Set `User`, `WorkingDirectory` and the two `ReadWritePaths`; the output goes to the journal (`journalctl -u wsrv-echo -f`).

Checked: `systemd-analyze verify` has no complaint, and the real worker ran under the same sandbox properties (a transient unit with `ProtectSystem=strict`, `PrivateTmp`, `PrivateDevices`, `NoNewPrivileges`, `SystemCallFilter=@system-service`, `RestrictAddressFamilies` and the rest) against a real daemon, with a browser connecting and joining. That run is why the unit does not set `MemoryDenyWriteExecute=yes`: PHP's regular-expression JIT needs executable memory, and PHP prints `Allocation of JIT memory failed`. The unit's comment says how to keep the setting (`php -d pcre.jit=0`, also checked). *Not run:* the unit as a system service (it needs root and a `wsrv` user), and `ProtectHome=yes` (the test files were under a home directory, so that run used `read-only`).

### Behind a proxy

pusher-js connects to `/app/<key>`, or to `<wsPath>/app/<key>` if you set Echo's `wsPath` (a pusher-js option that puts the WebSocket under a prefix; checked in the pusher-js 8.6.0 source). The daemon accepts the upgrade on any path. If your application has routes under `/app/`, give the WebSocket a prefix such as `wsPath: '/ws'` and proxy that. The proxy must pass the upgrade, keep the connection open longer than the 30-second ping, and say who the client is:

```nginx
location /app/ {                       # or /ws/ with wsPath: '/ws'
    proxy_pass http://127.0.0.1:8080;  # no path here: the request path is passed on as it is
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_read_timeout 120s;
    proxy_send_timeout 120s;
    proxy_buffering off;
}
```

With Caddy, `reverse_proxy /app/* 127.0.0.1:8080` does the same (the upgrade and `X-Forwarded-For` are automatic); other proxies: [DEPLOYMENT.md](DEPLOYMENT.md) section 5. **Set `WSRVD_TRUSTED_PROXY`** to the proxy's address (`127.0.0.1,::1` on the same host), or the daemon sees every browser as the proxy: it allows 100 live connections and 50 new ones a second per address and answers the rest with `429` ([OPERATIONS.md](OPERATIONS.md) section 1.2; `wsrv_connections_total{result="rejected_ip_connections"}` counts them). `allow_origin` is the page's origin, not the proxy's. *Not run:* nginx and Caddy are not installed on the machine this was tested on, so these snippets are DEPLOYMENT.md's with the path changed; what the daemon does with `X-Forwarded-For` is covered by its own tests (ADR-0029).

### Docker, Sail and Herd

- A **Unix socket** needs the worker and the daemon on one kernel and as the same user: in containers, share a volume for the socket and run both as uid 10001 ([examples/docker](../examples/docker)).
- Over **TCP** the worker can be anywhere. The daemon gets `WSRVD_IPC_PORT=9300`, `WSRVD_IPC_HOST=0.0.0.0` (inside a container, so that a published port arrives) and `WSRVD_IPC_TOKEN_FILE`; the Laravel connection gets `'ipc_endpoint' => 'tcp://wsrvd:9300'` and `'ipc_token_file' => env('WSRV_IPC_TOKEN_FILE')`. The token must be one `wsrvd --generate-token=FILE` made, and the link is **not encrypted**: keep it on a private network or a loopback address. The worker reads the token file as its own user, while the daemon insists on a file that only its own user (or root) can read, so give the worker a copy it can read or run it as the daemon's user. [examples/host-worker](../examples/host-worker) is the daemon side.
- **Sail:** add a `wsrvd` service to `compose.yaml`, point `WSRV_IPC` at it, and run `sail artisan wsrv:echo` (or give it a service of its own with that command and `restart: unless-stopped`). **Herd** (PHP on the host, the daemon in Docker): publish `127.0.0.1:9300:9300` and set `WSRV_IPC=tcp://127.0.0.1:9300`.
- *Run:* `php artisan wsrv:echo` inside a Laravel 13.43 container (PHP 8.3) reaching a daemon on the host over TCP through `host.docker.internal` with a generated token; a browser connected, joined a public channel and was answered, and the statistics line counted it. *Not run:* Sail and Herd themselves, Compose service names, macOS and Windows hosts.

### Size

One worker is one PHP process on one core. In the load runs (section 12) it used about 3 KB of PHP memory per connection with four channels each (20 000 browsers: 59 MB), on top of what your application needs to boot (about 25 MB in the Laravel container above), and one core carried 78 000 whisper frames a second. PHP's `memory_limit` applies to the worker: check `php -i | grep memory_limit` (it is unlimited on the test machine's CLI; `php.ini` files often set 128M) and set what you need with `php -d memory_limit=512M artisan wsrv:echo`. The worker holds one descriptor for its link to the daemon, not one per browser; the daemon's `LimitNOFILE` is the one that counts ([DEPLOYMENT.md](DEPLOYMENT.md)).

## 10. Watching the gateway

`wsrv:echo` writes one line to standard error every `--stats-interval` seconds (default 60, `0` turns it off):

```
[wsrv] gateway connections=200 channels=600 joins_in_progress=0 presence_channels=4 presence_members=200 memory_bytes=1597248 opened=200 frames=600 joins=600 bad_frames=0 unauthorized=0 rate_limited=0 subscription_errors=0 daemon_refused=0 strike_closes=0
```

The line is `key=value` pairs, numbers only (nothing a browser sent can end up in it), in this fixed order, so `grep -o 'daemon_refused=[0-9]*'` is all a log shipper needs. For your own exporter, `Gateway::stats()` returns the same as an object (`toArray()`, `line()`) and `Worker::every($seconds, $callback)` runs a function on a timer inside the worker. The gauges say how things are now. The counters count since the worker connected to the daemon, and start again from zero whenever it does.

| Field | Kind | Meaning |
|---|---|---|
| `connections` | gauge | browsers the gateway serves now; it matches the daemon's `wsrv_connections_current` (51 and 51 in a check). It is lower for up to `shutdown_grace_ms` after a worker restart, while the daemon still holds the previous worker's browsers (3 against 0 in a check); a number that stays above the daemon's is the sign of a leak |
| `channels` | gauge | (browser, channel) memberships now |
| `joins_in_progress` | gauge | joins sent to the daemon and not yet answered |
| `presence_channels`, `presence_members` | gauge | presence rooms and the users in them (a user with two tabs counts once) |
| `memory_bytes` | gauge | `memory_get_usage()` of the whole worker process, your application included |
| `opened` | counter | browser connections opened |
| `frames` | counter | frames received from browsers |
| `joins` | counter | joins answered with success |
| `bad_frames` | counter | frames that are not valid Pusher protocol (each costs a strike) |
| `unauthorized` | counter | signatures that did not verify (`4009`) |
| `rate_limited` | counter | frames over the message rate (`4301`, a strike) |
| `subscription_errors` | counter | joins answered with `pusher:subscription_error`: over the channel quota, refused by the daemon, or a member list that did not fit |
| `daemon_refused` | counter | joins the daemon's broker limits refused (section 2) |
| `strike_closes` | counter | connections closed with 1008 for strikes |

What to watch:

| You see | It means |
|---|---|
| `connections` stays above the daemon's `wsrv_connections_current` | the gateway holds state for browsers that are gone: a leak, please report it. In every run of section 12, including the churn run with killed clients, all gauges returned to zero |
| `joins_in_progress` above 0 for minutes | joins waiting for a daemon that does not answer |
| `daemon_refused` rising | the daemon's broker limits are too small for the site (section 2), or something is naming junk channels |
| `unauthorized` jumps after a deploy | the worker and the web application disagree on the key or the secret: restart the worker. Rising alone: someone is forging signatures |
| `rate_limited` and `strike_closes` rising | a client sending more than `messages_per_second`: a script, an attack, or an application that whispers faster than the limit (section 7) |
| `opened` starts again from 0 | the worker restarted or reconnected to the daemon |
| `memory_bytes` rising while `connections` is flat | report it; PHP does not give freed memory back, so compare over hours, not minutes |

What the daemon says about itself is on its admin port (`/metrics`: `wsrv_connections_current`, `wsrv_connections_total{result}`, `wsrv_close_total{class}` and more), see [OPERATIONS.md](OPERATIONS.md).

## 11. When something does not work

| What you see | Why | What to do |
|---|---|---|
| The browser's WebSocket fails at once | the `Origin` is not in `allow_origin` (the daemon logs `origin_refused`), the proxy does not pass `Upgrade`, or the port or scheme is wrong (`forceTLS`, `wsPort`, `wssPort`) | `journalctl -u wsrvd -g origin_refused`; check the address in the browser's network tab against sections 5 and 9 |
| The socket opens (HTTP 101) but Echo never connects | no `wsrv:echo` is attached: the daemon holds the connection and sends nothing. The moment a worker attaches, the browser receives `pusher:connection_established`. (With `check_app_key` the daemon refuses with `503` instead, and logs `handshake_undecided`: the next row.) | start the worker; the daemon logs `app_connected` |
| The browser's WebSocket fails with `503` while the worker is down | `check_app_key` is on, so the daemon runs the handshake hook and no worker can answer it; the daemon logs `handshake_undecided` | start the worker; it connects and the browsers get in. Leave `check_app_key` off if browsers should wait for a worker instead |
| `wsrv:echo` prints `wsrvd still has the previous application connected, retrying` | another worker is attached to the daemon (a second unit, a forgotten terminal, a second container), or the previous one died and the daemon has not noticed yet | find and stop the other one; the worker keeps retrying by itself |
| `wsrv:echo` prints `cannot reach wsrvd: ...` and retries | nothing is listening at `ipc_endpoint` (`Connection refused`), the path is wrong, or, with a Unix socket, the worker runs as another user than the daemon | check `ipc_endpoint`; run the worker as the daemon's user, or use TCP with a token (section 9) |
| `wsrv:echo` stops at start with an error that names a setting | the connection is checked as a whole when it is read: a missing publish token (the worker needs it too), a secret under 16 bytes, a misspelled option. The error never shows a secret | fix the setting it names |
| Every private and presence join fails with `4009` (`unauthorized` rises) | the signature does not verify: the worker has another key or secret than the web application (a rotated secret, a stale `config:cache`, a worker that was not restarted) | restart `wsrv:echo` with the web servers; compare `WSRV_APP_KEY` with Echo's `key` |
| `Echo.private()` never subscribes and the network tab shows a failed `POST /broadcasting/auth` | that route is Laravel's, not wsrv's. 403: the channel callback returned false or nobody is logged in. 419: CSRF or session. 404: the broadcasting routes are not registered. 500: your Laravel log | fix it as for any Laravel broadcaster |
| `toOthers()` still delivers to the sender | Laravel leaves a browser out by its socket id, which must arrive in the `X-Socket-ID` header of the request that broadcasts. Echo adds it to requests made through axios, jQuery, Vue or Turbo, not to plain `fetch` (checked in the laravel-echo 2.6.3 source) | send `'X-Socket-ID': Echo.socketId()` yourself |
| `broadcast(new Event)` returns and nothing arrives | an event that `implements ShouldBroadcast` is queued: nothing is published until a queue worker runs it | run `php artisan queue:work`, or make the event `ShouldBroadcastNow` |
| `BroadcastException: wsrv publish failed: ...` | `cannot reach the publish endpoint`: the daemon is down or `publish_url` is wrong. `publish refused with 401`: the token is not the daemon's (it logs `publish_rejected`). `publish refused with 413: body too large`: the event is over `broker_max_payload_bytes` (64 KiB by default) | a queued event is retried by your queue's rules; a `ShouldBroadcastNow` event throws inside the request |
| Joins fail with `LimitReached` for a user far below 64 channels, `daemon_refused` rises | the daemon's broker limits are reached (`broker_max_topics` is 1 024 by default) | size them (section 2) |
| A presence join fails with "The member list is too large to send" | the member list does not fit one frame to the daemon | section 2: raise `ipc_max_frame`, lower `max_presence_members` or the size of `user_info` |
| Browsers disconnect and reconnect together | the worker or the daemon restarted. When the worker leaves, the daemon closes its browsers with 1012; `opened` starts again from 0 | expected during a deploy; otherwise read `journalctl -u wsrv-echo` and `-u wsrvd` |
| One browser is closed with 1008 and returns about a second later | it sent frames faster than the message rate for long enough to use up its strikes (`rate_limited`, `strike_closes`), or sent malformed frames (`bad_frames`) | section 7: raise `messages_per_second` and `client_events_per_second` together if your application really whispers that fast |
| Only about a hundred browsers can connect, the rest get `429` | behind a proxy without `trusted_proxy`, every browser is the proxy's address, and the daemon allows 100 live connections and 50 new ones a second per address | set `WSRVD_TRUSTED_PROXY` (section 9) |
| Browsers drop at regular intervals through a proxy | the proxy's idle timeout is shorter than the 30-second ping | `proxy_read_timeout 120s` or the equivalent |
| The worker's memory grows and does not come back | PHP keeps what it grew to: in the churn run `memory_bytes` ended near, not at, its start. A rise while `connections` is flat is not normal | watch `memory_bytes` against `connections` over hours (section 10) |

## 12. What was tested, and how to run it

| Run | What it covers | Where |
|---|---|---|
| Unit tests (`php tests/run.php`) | frames, signatures (with Pusher's published vectors), presence, every limit and the forgiveness of strikes, the statistics, hostile input, a fuzz, the config checker, the supervisor | CI |
| `php tests/pusher_interop.php WSRVD` | a real `wsrvd`, the gateway and raw WebSocket clients: join, private signatures, presence, whispers, `toOthers`, hostile frames, quotas, `check_app_key`, 64 channels subscribed in one burst with the default limits, and a presence member list that does not fit the daemon's frame (that join fails, nothing else does) | CI (`ctest -R php_pusher_interop`) |
| `php tests/laravel_check.php APP` | the driver, `/broadcasting/auth`, `Broadcast::channel()`, `broadcast()->toOthers()`, encrypted channels and the artisan command inside a real Laravel 13 application | by hand (needs an application with `vendor/`) |
| `bash tests/echo_browser/laravel/run.sh` (settings in its header) | the same scenario with the whole product: a real Laravel 13 application (a copy of a skeleton is used, never modified) with its session, CSRF and `auth` middleware, `Broadcast::routes`, the `wsrv` driver through `broadcast(...)->toOthers()`, the documented `config/broadcasting.php`, `php artisan wsrv:echo` (its supervisor reconnects after the daemon restart by itself), a real `wsrvd`, and the real libraries in Chrome | by hand (needs a Laravel 11+ application on disk, the two npm bundles and a browser) |
| `python3 tests/gateway_load/run.py --daemon WSRVD --scenario join\|whisper\|churn` | thousands of Pusher-protocol browsers against a real `wsrvd` and the worker: reconnect storms, whisper fan-out, and churn with abusive clients and killed load processes; it fails unless every subscribe was answered and, after the last browser has gone, every gauge of the gateway is back at zero ([README](../bindings/php/tests/gateway_load/README.md)) | by hand (needs Node 22.4 or newer) |
| `php tests/echo_browser/run.php WSRVD PUSHER_JS ECHO_JS` | the real `pusher-js` 8.6.0 and `laravel-echo` 2.6.3 in Chrome against a real `wsrvd`: connect, public, private (signature through an auth endpoint, and a refused one), presence `here`/`joining`/`leaving`, whispers, `toOthers`, encrypted channels decrypted in the browser, a join followed at once by a leave, a clean disconnect, a page that joins 60 channels in one burst, and a restart of the daemon and the worker under connected browsers (Echo reconnects, re-authorizes and resubscribes, including that page's 60 channels) | by hand (needs the two npm packages and a browser) |

The `run.php` variant uses a small stand-in for your Laravel routes (`tests/echo_browser/router.php`), so it needs no Laravel; `laravel/run.sh` runs the same page against a real application. Last runs: 12 of 12 checks passed in each (2026-11-27, Chrome, `wsrvd` built from this branch; the Laravel run on Laravel Framework 13.43.0).

### Measured

One machine: an Intel Core i7-13700H laptop (14 cores, 20 threads, 15 GB), Linux, no containers. A release build of `wsrvd` on two pinned cores, one PHP 8.5 worker (CLI, no opcache) on one pinned core, and eight cores for the Node load generators; loopback; the bare gateway worker (`gateway_load/worker.php`), not a Laravel application; the daemon's broker limits raised as in section 2.

| Run | Result |
|---|---|
| 5 000 browsers connect at once and join 4 channels each (2 public, 1 private, 1 presence room of about 50): 20 000 joins | all answered, the last browser ready 1.8 s after it connected; 9 900 joins a second at the busiest 2-second sample; the worker 6% of a core on average (41% at the peak), 47 MB resident (33 MB at the start); the daemon 53 MB |
| 20 000 browsers, 80 000 joins | all answered, ready within 7.3 s; 22 000 joins a second at the busiest sample; the worker 16% on average, 49% at the peak, 89 MB resident, 59 MB of PHP memory at the peak and 5.6 MB after; the daemon 192 MB |
| Whispers: 2 000 connections in 400 private channels of 5, each member whispering 10 times a second for 20 s | 20 000 frames a second through the worker (39% of a core); 1 582 208 deliveries, none lost; latency p50 2 to 3 ms, p95 15 to 16 ms, p99 24 to 26 ms (the worst 68 ms), by load process |
| The whisper ceiling: 2 000 connections in 1 000 private channels of 2, each member 40 a second, limits raised | 78 000 frames a second (the worker's core 84% busy at the peak); 1 542 006 deliveries, none lost; p50 1 ms, p95 2 ms, p99 5 to 9 ms |
| Churn for 2 minutes: 200 loops in 4 processes of connect, join random channels, whisper, leave, disconnect (a sixth of them before the answer), 5% of the connections abusive, a load process killed with SIGKILL every 12 s (9 times) | 36 149 connections, 103 159 joins, 1 544 connections closed for strikes; after the last client every gauge was 0; PHP memory 0.86 MB at the start, 1.5 MB at the peak, 1.06 MB at the end; resident memory 32.8 MB to 33.7 MB |
| 3 000 browsers against the daemon's default `broker_max_topics` (1 024) | 3 918 of 12 000 joins refused with `LimitReached`; no browser lost, gauges back to 0 |

These say how much one PHP process and a two-thread daemon do on one box. They say nothing about a network, TLS, a proxy, or your application's own `/broadcasting/auth` route, which every private and presence join also costs. Re-run them on your hardware with `bindings/php/tests/gateway_load/` before you size a server from them.

**Not run:** Safari and Firefox; Echo's `pusher` broadcaster; Laravel 11 and 12 (13.43 was used; the broadcaster uses only the long-stable `Broadcaster` contract, and `laravel_check.php` runs against any version you have); more than 20 000 connections; a network, TLS or a proxy between the browsers and the daemon; macOS and Windows hosts; `Echo.user()` and the watchlist, which are not supported.
