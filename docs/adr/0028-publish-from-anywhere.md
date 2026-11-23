# ADR-0028: Publish from anywhere: rooms and an HTTP publish endpoint for `wsrvd`

- Status: **Proposed** (2026-11-23; the owner delegated the build order on 2026-11-23, the owner moves it to Accepted)
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant

## Context
The sidecar's selling point for PHP is "your normal PHP-FPM app pushes a message to browsers". Today it cannot:
- `wsrvd` takes **one** IPC connection (ADR-0017), owned by the worker process, and only that process knows connection ids. A controller that wants to say "tell everyone in room 7" has nothing to address.
- The IPC socket is `0600` with a peer-uid check, so a PHP-FPM pool running as another user cannot connect, and in containers the PHP-FPM and daemon containers rarely share a uid.
- The broker (ADR-0024) exists in C++ only; ADR-0027 (Proposed) brings it to the C ABI but explicitly leaves the sidecar out (its section 7).

What the established servers do (Pusher, Laravel Reverb, Centrifugo): the browser subscribes over the WebSocket, and server-side code publishes through a small **authenticated HTTP API**. HTTP is the one transport every PHP stack, queue worker, cron job and other language can already speak, across containers and hosts, with no shared uid and no long-lived connection.

## Options considered
1. **A publish command on the IPC socket, used by FPM requests.** Needs a second concurrent IPC client, a different peer-uid rule (a shared group or mode, which ADR-0017 deferred) and a connection per request. Rejected: it weakens the best property of the IPC (private to one uid) and does not cross containers.
2. **An HTTP publish endpoint on its own listener** (this ADR). Bearer-token authenticated, bounded, off by default. Reuses the broker and the existing HTTP parser and listener code.
3. **Redis or another broker as the bridge** (what Reverb scaling uses). A new dependency and a second thing to run; ADR-0005 forbids it for the core. A bridge can be built later by an application on top of option 2.

## Decision

### 1. Layers, in the order they are built
1. **ADR-0027 slices 1 and 2** (broker through the C ABI, ABI 0.5), unchanged. `wsrvd` links the same `EmbeddedServer`, so it gets the broker for free.
2. **IPC v0.3** (this ADR, section 2): the worker can subscribe and unsubscribe its connections, and publish.
3. **The publish endpoint** (section 3): any process publishes over HTTP.
4. **PHP** (section 4): `Connection::subscribe()`, `Wsrv\Publisher`, and a Laravel guide.

### 2. IPC v0.3: three frames (additive; minor 3)
| type | name | body |
|---|---|---|
| 12 | `SUBSCRIBE` | `u32 request, u64 conn, topic` |
| 13 | `UNSUBSCRIBE` | `u32 request, u64 conn, topic` |
| 14 | `PUBLISH` | `u32 request, u8 kind (1 text, 2 binary), u64 except (0 = nobody), u16 topic_len, topic, payload` |

Each is answered by a `STATUS` with the command's `request` and the C ABI's `wsrv_status` (ADR-0027 section 3, including the new `LIMIT`). The authority to subscribe is the **worker's**: the browser asks over the WebSocket in the application's own protocol, the PHP code decides (it has the session), then calls `subscribe`. The daemon never reads a topic from a browser. A daemon without `broker = true` answers `INVALID_ARGUMENT`. The library removes a connection's subscriptions when it closes (ADR-0027 section 4), so the worker cannot leak.

### 3. The publish endpoint
Off unless `publish_port` is set (`wsrvd --publish-port=9200`); bound to `publish_host`, default `127.0.0.1`. It runs on loop 0 beside the admin listener and reuses its bounded client machinery.

```
POST /publish?topic=room.7[&except=<conn id>][&binary=1]
Authorization: Bearer <token>
Content-Length: <n>

<the message bytes>
```
- **No JSON.** The body is the message itself, so the daemon needs no parser beyond what exists, and an application sends whatever it already has (typically a JSON string it built). `Content-Length` is required; chunked bodies are refused (`411`/`501`).
- **Answers:** `200` with `subscribers=<n> accepted=<n> refused=<n>` (text, the `PublishReport`), `400` bad topic or payload, `401` missing or wrong token, `404` any other target, `405` other methods, `411` no length, `413` over the broker payload bound, `415` text publish whose body is not UTF-8 (`422` reserved), `429` the publish queue is full, `503` broker off or shutting down.
- **The token is required.** The endpoint does not start without `publish_token_file` (mode `0600`, owned by the service user, at least 32 bytes), compared in constant time. There is no command-line or config value for the token (the rule of ADR-0017). A token in the file is the only authority; it is the application's secret, the browsers never see it.
- **Bounds (SECURITY_DESIGN gets a new row, T22):** at most 16 simultaneous clients, request head at most 4 KiB, 5 s to send head and body, one request per connection (`Connection: close`), body at most `broker_max_payload_bytes` (64 KiB default). The handler does one `publish` call, which already isolates slow subscribers (ADR-0024). A non-loopback `publish_host` logs a warning and the documentation says to put TLS in front (a reverse proxy, or a private network); `publish_*` keys never default on.
- **No wildcard, no list-topics, no read endpoints.** The endpoint can only publish. Reading or administering stays on the admin listener (loopback, unauthenticated, ADR-0023) or the IPC.

### 4. PHP
- `Wsrv\Publisher` (pure PHP, `stream_socket_client` or the `http` stream wrapper, no ext needed): `new Publisher('http://wsrvd:9200', $token)`, `->publish('room.7', $json, except: $conn)`. Throws `PublishFailed` with the status on a non-200. Five lines from a Laravel controller or a queued job.
- `Connection::subscribe($topic)` / `unsubscribe`, `Worker::publish()` for the IPC path, `LimitReached` exception (ADR-0027 section 6).
- A Laravel guide first (a plain service class), a `wsrv/laravel` package (a Broadcaster driver) only if the guide shows real friction. Not built speculatively.

### 5. What this ADR does not do
- Authenticating browsers to **topics**: the worker's job (section 2). A "private channel" helper is a PHP-side convenience later, not a daemon feature.
- Cross-instance delivery (two `wsrvd`s): the application publishes to each, or uses its own bridge (Option 3). Stated in OPERATIONS.
- Presence, history, acknowledgements, rate limits per token: not in 1.0.

## Slices
1. ADR-0027 slices 1 and 2 (C++ `HostBridge`, C ABI 0.5), then slice 3 of 0027 (PHP embedded) is optional and can follow.
2. IPC v0.3 frames in `src/ipc/` (codec, fuzz target extended), `wsrvd` relay, the loopback integration test, IPC_PROTOCOL 0.3.
3. Publish endpoint: parser reuse, token check, bounds, tests (hostile requests: oversize head, slowloris, wrong token, chunked, bad topic), metrics `wsrv_publish_http_total{status}`.
4. PHP `Publisher`, `subscribe`, tests against a real `wsrvd`, the Laravel guide, the compose example gains a publish demo, lesson.

## Consequences
- A controller, a queue job or a cron script can push to browsers with one HTTP call. The headline use case works on plain PHP-FPM.
- One more listener and one secret to protect: bounded, off by default, token required, documented.
- The IPC grows by three frames; a v0.2 worker keeps working (minor 3 is additive; a daemon refuses commands 12 to 14 from a worker that said minor below 3).
- Revisit if per-topic publish permissions are needed (several applications sharing one daemon), if TLS on the publish listener is demanded (then reuse `TlsServerContext`, ADR-0019), or if JSON bodies with metadata (ids, tags) prove necessary.
