# ADR-0042: Laravel Echo support, spoken by the PHP worker, not by the daemon

- Status: **Accepted** (2026-11-27, by the project owner in chat: "yeah go ahead, accepted, and also we should ship a completed production on the first release"). Amends ADR-0028 section 4 (see "What changed since ADR-0028"). Amendment A1 records what "completed" means for the first release.
- Date: 2026-11-27
- Deciders: project owner, maintainer assistant

## Context
A Laravel developer who wants real-time features does three things today: `broadcast(new OrderShipped)` on the server, `Echo.private('orders.1').listen(...)` in the browser, and one `.env` line to pick the server. wsrv has none of that (docs/LARAVEL.md "Honest scope"): the developer writes a worker, a ticket login and calls `Wsrv\Publisher` by hand. That is the largest remaining reason a Laravel project would not try wsrv, and the roadmap (owner, 2026-11-26) puts "Laravel and Echo" after the adoption layer.

Laravel's Echo client speaks the **Pusher WebSocket protocol** (through the `pusher-js` library). Laravel's server side is a *Broadcaster*: a class with `broadcast(channels, event, payload)` and an authentication method for private and presence channels. So there are two interfaces to meet: a wire protocol in the browser, and a PHP class on the server.

What the repository already gives (verified in the code, 2026-11-27):

- The daemon accepts **any origin-form request target** on the WebSocket listener (`/app/KEY?protocol=7` passes; nothing routes on the path). With `handshake_hook=true` the worker also sees the path and the raw query (src/server/handshake_decider.hpp).
- The worker **can send a text frame from its `open` handler** (the daemon queues the 101 first), can **subscribe a connection to a room** and unsubscribe it, can send to one connection, and the daemon fans a published message out to a room without the worker (ADR-0024, ADR-0028). One worker attaches per daemon.
- A topic name is 1 to 128 bytes of `[A-Za-z0-9._:/@-]`. `private-`, `presence-`, `private-encrypted-` and `App.Models.User.1` pass. Pusher also allows `=`, `,` and `;`, and names up to 164 bytes; wsrv refuses those.
- Limits already exist and are global: 64 topics per connection, 10 000 subscribers per topic, 100 000 subscriptions, 1 024 topics, 64 KiB per published payload (all configurable). A refused subscribe comes back as `LIMIT`.
- `except` (a connection id to leave out) is a `uint64` on the wire and an `int` in PHP. This is what Laravel's `toOthers()` needs.
- The C++ code has **no HMAC, SHA-256 or MD5** (`src/crypto` has SHA-1 and Base64 only, for the handshake). PHP has `hash_hmac` and `hash_equals`.
- There is no list-members command anywhere in the IPC or the daemon (ADR-0028: "not in 1.0"). Presence state must be kept by the application.
- When the worker disconnects, its rooms are dropped at once and its connections are closed with 1012 after the grace period (IPC_PROTOCOL section 6). So worker state and connection state die together, which makes worker-held presence state coherent.

## Decision

### 1. Where the Pusher protocol is spoken: in the PHP worker (option A2)
A new part of the PHP package, `Wsrv\Pusher\Gateway`, is a worker plugin. It answers the Pusher WebSocket protocol using only commands the daemon already has. **The daemon, the IPC protocol and the C ABI do not change.**

What it does, per connection:

1. On `open`: send `pusher:connection_established` with a `socket_id` and an `activity_timeout`.
2. On a `pusher:subscribe` frame: for a **public** channel, `subscribe($channel)` and answer `pusher_internal:subscription_succeeded`. For a **private** or **presence** channel, first verify the signature (section 3), then subscribe. A bad signature, a refused name or a `LIMIT` answers a `pusher:error` and subscribes nothing.
3. On `pusher:unsubscribe`: `unsubscribe`. On `pusher:ping`: `pusher:pong`.
4. Presence channels: keep the member table (section 4) and publish `pusher_internal:member_added` and `member_removed`.
5. On `close`: drop the connection's presence entries and publish the `member_removed` events. Room subscriptions end by themselves (the daemon does it).

**Fan-out stays in C++.** A broadcast is one `publish` to a room; the worker is only on the path for connect, subscribe, presence and close, never per delivered message. This keeps the benchmark story intact (ADR-0028 and BENCHMARKS.md measure that path).

### 2. The server side: `Wsrv\Laravel\WsrvBroadcaster`
A Laravel Broadcaster (`BROADCAST_CONNECTION=wsrv`) with two jobs:

- `broadcast($channels, $event, $payload)`: for each channel, `Publisher::publish($channel, $json, except: $conn)`, where `$json` is the Pusher event envelope and `$conn` is decoded from the `socket` Laravel passes for `toOthers()` (the `socket_id`, which carries the connection id).
- `auth($request)` and `validAuthenticationResponse(...)`: run Laravel's own channel authorisation callbacks (`routes/channels.php`), then return the signed response `pusher-js` expects. Laravel's `/broadcasting/auth` route keeps working unchanged. The signature is `hash_hmac('sha256', ...)` with the app secret, in PHP.

The app key and secret are ordinary Laravel configuration shared by the Broadcaster (which signs) and the Gateway (which verifies): the Pusher model. The Gateway takes the secret from the code that builds it (read from the environment or a mode-0600 file by the application), never from a command line.

### 3. Security rules (each gets a test)
1. **Signatures** are compared with `hash_equals`. A private or presence subscribe without a valid `auth` is refused and subscribes nothing. A signature is bound to the `socket_id`, the channel and (presence) the `channel_data` string, so it cannot be replayed on another connection or channel.
2. **`socket_id` is built by the gateway** from the connection id and a random part, and sent in `connection_established`; a client never chooses it. The connection id is read back from a `socket_id` only for `except`, and a forged one can at worst leave one other connection out of one broadcast, because every signature is bound to the real `socket_id` of the subscribing connection.
3. **Every inbound frame is untrusted JSON.** The daemon's `max_message_size` already bounds it (recommended 16 KiB for a Pusher deployment; documented). The Gateway additionally decodes with a depth limit, requires `event` to be a string and `data` to be an object or a string, and answers anything else with `pusher:error` without throwing. A handler that throws would be reported by the Supervisor and not take the link down, but a gateway must not rely on that.
4. **Name policy:** a channel name that wsrv cannot carry (`=`, `,`, `;`, longer than 128 bytes) is refused with an error frame, never rewritten. A rewrite would let two Pusher names collide on one topic.
5. **Presence bounds:** `channel_data` at most 1 KiB per member (the Pusher figure, to verify), at most 100 members per presence channel by default (configurable), and one entry per user id per channel (a second connection of the same user adds a connection, not a member).
6. **`client-*` events (whispers) are not supported in the first release.** A frame for one is answered with an error. They need rate limits and a decision about trust; a later amendment may add them, off by default.
7. **No encrypted private channels** (`private-encrypted-`): refused with an error. The server cannot read what the client encrypts, and the feature needs key handling that this ADR does not take on.
8. **The app secret is never logged** and never appears in an exception message.

### 4. Presence state: per daemon, held in the worker
A `members` table per presence channel, keyed by user id, with a reference count of connections. It lives in worker memory. Consequences, stated in the guide:

- With one worker per daemon (the only supported layout) it is exact for the connections that daemon holds.
- With several daemons (DEPLOYMENT.md section 7) each daemon sees only its own members. Presence across nodes needs a backplane, which does not exist (planned 1.x). Until then, the guide says so and recommends one daemon for presence channels.
- A worker restart closes its connections (1012 after the grace period), so no stale member list outlives the connections it described.

### 5. What does not change, and what will not work
- **The stock `pusher` and `reverb` Laravel drivers will not work** against wsrv: they call the Pusher HTTP API (`POST /apps/{id}/events`, signed with MD5 and HMAC), which wsrv does not implement. The driver is `wsrv`. Implementing the HTTP API is rejected for now (Alternatives).
- Delivery is at most once. A browser that reconnects gets no replay (IPC_PROTOCOL section 6), and `pusher-js` re-subscribes by itself.
- Origin checks, TLS, per-IP limits, the buffer budget and every other daemon control apply unchanged; the Gateway sits behind them.

### 6. Packaging, version, dependencies
- Code in `wsrv/client`: `Wsrv\Pusher\*` (framework-free: frames, signature, presence table, the Gateway) and `Wsrv\Laravel\*` (the Broadcaster, a service provider, the `wsrv:echo` Artisan command). Laravel is a `suggest`, not a `require`: a plain PHP user pays nothing.
- **No runtime dependency is added** (PHP `hash_hmac`, `json_decode`, `random_bytes`). The pure core is tested without Laravel installed. The Laravel glue is a thin layer and is tested the way the existing guide is: by hand against a real Laravel app, then in CI only if a development dependency on `illuminate/broadcasting` is approved separately (it is a download and an ADR-level dependency decision).
- **Version: 1.2.0** (a new feature in the PHP package; the daemon and the IPC protocol are unchanged, the C ABI stays 1.1).

### 7. Tests, and the gate for the word "works"
1. Unit tests for the pure core, replaying frames shaped like the ones `bench/compare/wsbench` already sends and expects against Reverb (`pusher:subscribe`, `connection_established`, `subscription_succeeded`, ping and pong), plus signature vectors, presence transitions, and a property test of random and malformed JSON asserting that no exception escapes and no bound is exceeded.
2. A test against a real `wsrvd` with a hand-written WebSocket client (the PHP package's tests already do this for the sidecar).
3. **An interoperability run with the real `pusher-js` and Laravel Echo** (npm, a few MB) is the gate before any document says "works with Laravel Echo". It needs a download, so it needs the owner's yes first (metered connection). Until it passes, the docs say "implemented to the Pusher protocol, not yet verified against `pusher-js`".

## Facts that were checked before the code, and what is still open
The first draft was written from recall. Before implementing, these were checked against sources that were on hand (Laravel Reverb's server and the `pusher-php-server` SDK, read from a local image; read only, nothing copied) and against the signature vectors Pusher publishes:

- Frame shape: `event`, `channel`, and `data` as a JSON-encoded *string* for server events; `socket_id` is `digits.digits`; `activity_timeout` is the number of seconds after which the client pings.
- Signatures: `key:` plus HMAC-SHA256 over `socket_id:channel` (presence: `socket_id:channel:channel_data`). The test suite carries the published vectors.
- Error codes and messages (4001, 4009, 4200, 4301, subscription error 4300) as Reverb sends them.
- Laravel's `Broadcaster` contract, `UsePusherChannelConventions`, the `socket` key of the payload and `BroadcastManager::extend`. Verified by running the glue against a real Laravel 13 application (`tests/laravel_check.php`).
- Encrypted channels: `shared_secret = base64(SHA-256(channel . master key))`, payloads as libsodium secretbox, as in the SDK.

The interop gate of section 7 was run on 2026-11-27: the real `pusher-js` 8.6.0 and `laravel-echo` 2.6.3, in Chrome, against a real `wsrvd` and the gateway (`tests/echo_browser/run.php`, 11 of 11 checks, including a restart of the daemon and the worker under connected browsers). It found no gateway bug, but it corrected two things I had assumed: `pusher-js` posts its auth request form-encoded, not JSON (only the test stub was wrong), and for a join followed at once by a leave it still sends `pusher:subscribe` and sends `pusher:unsubscribe` only after the success, so other members see a join and then a leave, as on Pusher itself. The same page was then run against a real Laravel 13.43 application (session, CSRF, `Broadcast::routes`, the `wsrv` driver, `php artisan wsrv:echo` reconnecting after the daemon restart) with a real `wsrvd`: 11 of 11 (`tests/echo_browser/laravel/run.sh`). Not run: other browsers, the `pusher` broadcaster of Echo, Laravel 11 and 12.

## Alternatives considered
1. **The daemon speaks Pusher (option A1).** Rejected. It needs JSON parsing of wire data, HMAC-SHA256 and MD5 in C++ (`src/crypto` has neither), presence state in the event loop, new config keys, new fuzz targets and an ABI decision, all on the code that holds every socket. The only gain is that a worker is not on the subscribe path; the benchmarks do not show that path as a bottleneck. Revisit only if a measured need appears.
2. **A custom Echo connector over `clients/js` (option B).** Not chosen as the primary path: every Laravel user would change their JavaScript and depend on a connector that is not the standard `pusher-js`. It remains possible later, cheaply, as a second door onto the same Gateway.
3. **Implement the Pusher HTTP API so the stock drivers work.** Rejected for now. It needs an HTTP listener (in the daemon: more C++ and MD5/HMAC; in the worker: a second event source in a loop that must stay responsive) and request signing. A Laravel Broadcaster is a few dozen lines; the HTTP API is a protocol. Revisit if users ask to keep the `pusher` driver.
4. **Do nothing; keep the guide.** This is what ADR-0028 chose, for lack of evidence of friction.

## What changed since ADR-0028
ADR-0028 section 4 said a Broadcaster driver would be built "only if the guide shows real friction. Not built speculatively." There are not yet users to show friction. What changed is the owner's roadmap (2026-11-26): Laravel and Echo support is a goal in itself, a reason to adopt, not a reaction to complaints. This ADR therefore **amends that sentence** (an amendment note is added to ADR-0028 when this ADR is Accepted). It does not make the claim that users asked for it.

## Consequences
- + A Laravel app gets `broadcast()`, `Echo.private()`, `Echo.join()` and presence by changing `.env` and one config block, with the daemon unchanged and fan-out still in C++.
- + No new attack surface in the process that holds the sockets.
- − Presence is per daemon. Several nodes need a backplane first.
- − `client-*` whispers, encrypted channels and the Pusher HTTP API are not there in 1.2.0.
- − One more thing the PHP worker does: per-subscribe signature checks cost a little CPU in the single worker (a subscribe, not a message, so it is bounded by connect rate).
- − Until the interop run passes, the claim is "implemented to the protocol", not "works".

## Amendment A1 (2026-11-27): decisions taken while implementing

The owner accepted this ADR and asked for a completed production release first, not a reduced first cut. These points were open or left out of the first text and are now decided:

1. **Client events (whispers) are in the first release.** Echo's `.whisper()` and `listenForWhisper()` send `client-*` events. They are on by default, only for a connection that is a member of a *private or presence* channel (never public), at most `client_events_per_second` (10) per connection, only with an object or array as data, and the relayed frame is built by the server: the event name, the channel and the *signed* user id (presence) are the server's, so a browser cannot forge another member's id or add fields. They are refused on `private-encrypted-*` channels, as `pusher-js` refuses them. They can be switched off (`client_events => false`), which answers with Pusher's 4301. Leaving a channel before the daemon has answered the join cancels the join (the daemon room is left again, nobody is announced).
2. **Encrypted channels (`private-encrypted-*`) are in the first release.** The broadcaster derives the shared secret and encrypts payloads with libsodium (`ext-sodium`, in PHP since 7.2). If the master key is not configured, broadcasting to such a channel is an error: the payload is never sent in the clear to a channel the browser expects to be encrypted.
3. **One daemon.** `toOthers()` leaves a connection out by the daemon's connection number inside the socket id, and presence is held per worker. Both are only correct with one daemon, so the broadcaster takes a `Wsrv\Publisher`, not a `ClusterPublisher`. Running several daemons with the Pusher protocol is not supported in 1.2 (a shared presence store would be a later ADR).
4. **Values JSON cannot carry back out.** A browser can send a number such as `1e999`, which decodes to infinity and cannot be re-encoded. The gateway treats it as a bad frame (4200, one strike); found by the commit security review, with tests.
5. **Limits are configuration, validated at start.** Every bound (frame size, channels per connection, pending joins, presence members, channel data size, message and whisper rates, strikes) has a default for a public site and a range check; an unknown option name is an error.
6. **Optional strict app key.** With `check_app_key` (daemon `handshake_hook`), a URL that does not end in `/app/<key>` is refused with 404 at the handshake. Off by default because the key is public and the signature is what protects private channels.
7. **User authentication (`pusher:signin`, Echo's `user()` and watchlist) is not supported.** It is a separate Pusher feature that Laravel's `resolveAuthenticatedUser` serves; the gateway answers a `pusher:signin` frame with a 4200 error, once, without a strike. Listed as a limit in the docs, not hidden.

