# Laravel Echo on wsrv

Use Laravel's own broadcasting (`broadcast(new Event)`, `Broadcast::channel()`, `routes/channels.php`, Laravel Echo in the browser) with `wsrvd` as the WebSocket server, so a Laravel app gets real-time events without a Node service, a paid broker or a second WebSocket server to learn.

> **Status.** Planned for wsrv 1.2 (ADR-0042). The gateway, the broadcaster, encrypted channels and `php artisan wsrv:echo` are implemented and tested: unit tests, a run against a real `wsrvd` with raw WebSocket clients, and a run of the glue inside a real Laravel 13 application (`bindings/php/tests/laravel_check.php`). The words "works with Laravel Echo" are held back until the real `pusher-js` and `laravel-echo` libraries have been run against it; see section 9 for the state of that run.

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

Run the daemon. Compared with [LARAVEL.md](LARAVEL.md) there is no handshake hook, and a smaller message limit is a good idea, because a Pusher frame is small:

```
wsrvd --ipc-socket=/run/wsrv/ipc.sock --listen-port=8080 \
      --allow-origin=https://app.example.com \
      --broker=true --publish-port=9200 --publish-token-file=/run/wsrv/publish.token \
      --max-message-size=16384
```

`--max-message-size` should be about the gateway's `max_frame_bytes` (default 16384). Make the publish token with `wsrvd --generate-token=/run/wsrv/publish.token`, as in [LARAVEL.md](LARAVEL.md).

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
    'ipc_endpoint' => env('WSRV_IPC', '/run/wsrv/ipc.sock'),   // or tcp://host:port with 'ipc_token_file'
    // 'encryption_master_key_base64' => env('WSRV_ENCRYPTION_KEY'),   // only for private-encrypted-* channels
    // 'options' => ['max_presence_members' => 100, 'client_events' => true],   // see section 7
],
```

The configuration is checked when it is read: a missing key, a secret under 16 bytes, an unknown key or option name (a typo does not silently fall back to a default), or an unreadable token file is an error that names the setting and never prints a secret.

Then enable the broadcasting routes as usual (`php artisan install:broadcasting`, or `Broadcast::routes()` in your application) and define channels in `routes/channels.php` exactly as you would for Pusher.

## 4. Run the gateway

```
php artisan wsrv:echo
```

Run one per daemon, under systemd, Supervisor or a container restart policy. It reconnects by itself when the daemon restarts. When the daemon goes away it closes every browser; Echo reconnects and resubscribes, and the new gateway starts with an empty member list, so no presence state outlives the connections it describes.

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

The browser's `Origin` must be in `--allow-origin`. Channel names are limited to letters, digits and `. _ : / @ -` (up to 128 bytes); a name with `=`, `,` or `;` is refused with an error, never rewritten.

## 6. What works, and what does not

| Feature | In wsrv 1.2 |
|---|---|
| Public channels, `broadcast()`, `broadcastAs()`, `broadcastWith()` | yes |
| Private channels and `Broadcast::channel()` callbacks | yes: the signature is bound to the browser's socket id and the channel |
| Presence channels: `here`, `joining`, `leaving` | yes, per daemon; a user with two tabs is one member until the last tab leaves |
| `toOthers()` | yes, through the sender's socket id |
| Whispers (`.whisper()`, `listenForWhisper()`) | yes, only between members of a private or presence channel, rate limited |
| Encrypted channels (`private-encrypted-*`) | yes, with `ext-sodium` and a master key; without a key, broadcasting to one is an error and never sends plaintext |
| Pusher's HTTP API (`pusher/pusher-php-server` pointed at wsrv, `/apps/.../events`) | no |
| User authentication and watchlist (`pusher:signin`, `Echo.user()`) | no |
| More than one daemon with the Pusher protocol | no: presence and `toOthers()` are per daemon |
| Delivery to a browser that is offline | no: delivery is at most once; fetch what was missed from your database |

## 7. Limits and options

Every bound has a default for a public site, and each is range-checked. They are set under `'options'`:

| Option | Default | Meaning |
|---|---|---|
| `activity_timeout` | 30 | seconds of silence before the browser pings |
| `max_frame_bytes` | 16384 | a larger frame from a browser is a bad frame |
| `max_channels_per_connection` | 64 | the daemon's own `broker_max_topics_per_connection` (config file, default 64) must be at least this |
| `max_pending_per_connection` | 8 | subscriptions waiting for the daemon's answer |
| `max_presence_members` | 100 | users in one presence channel (a user with two tabs counts once) |
| `max_channel_data_bytes` | 2048 | the signed member data of a presence member |
| `client_events` | true | allow whispers |
| `client_events_per_second` | 10 | whispers per connection |
| `messages_per_second` | 30 | all frames per connection |
| `max_strikes` | 8 | bad frames before the connection is closed with 1008 |

A frame that is not valid JSON, has no event, carries a channel name wsrv cannot hold, or holds a number JSON cannot carry back out (such as `1e999`) is answered with Pusher's `4200` error and counts as a strike. A signature that does not verify is answered with `4009`.

## 8. Security notes

- **The app key is public**; it is in your JavaScript. The **secret** never leaves the server, is hidden from `print_r`, `var_dump` and `serialize`, and is used only to sign and to verify.
- A private channel signature covers the browser's socket id and the channel name. Replaying it from another connection fails, and the daemon, not the gateway, is the authority on which connection a socket id belongs to.
- A presence member's user id and info are **inside the signed data**. A browser cannot join as someone else, and a whisper carries the signed user id, never one the browser sent.
- Signatures are compared with `hash_equals`.
- `check_app_key => true` (with the daemon's `--handshake-hook=true`) refuses a URL that does not end in `/app/<key>`; it is off by default, because the key is public and the signature is what protects a channel.
- The publish endpoint (port 9200) is for your app servers only: never expose it to the internet.

## 9. Testing the glue

```
php tests/pusher_interop.php build/dev/apps/wsrvd/wsrvd      # a real wsrvd, the gateway, raw WebSocket clients
php tests/laravel_check.php /path/to/a/laravel-app            # the driver, the auth route and toOthers() in a real Laravel app
```

The first runs in CI (`ctest -R php_pusher_interop`). The second needs a Laravel application with `vendor/` installed, so CI does not run it.

State of the browser-library run (`pusher-js` and `laravel-echo` against this gateway): not yet done; this guide is updated when it is.
