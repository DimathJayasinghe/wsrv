# Operations

How the server is configured, observed, run, and shut down.

## 1. Configuration

One list of keys, three ways to set it. `wsrvd` takes flags (`--listen-port=8080`), a file (`--config=FILE`, one `key = value` per line, `#` comments) and environment variables (`WSRVD_LISTEN_PORT=8080`; a repeatable key takes a comma list). The embedded server (PHP FFI, C API) takes the same keys as text (`new Server(['listen_port' => 8080])`, `wsrv_config_set`). There is no TOML or YAML file, no `WSRV_*` variable and no config directory.

Precedence, lowest to highest: built-in defaults, then the environment, then `--config` files and flags in the order given (later wins). The configuration is checked once at start: a wrong key or value stops the start with exit code 1 and names the setting (and, for the environment, the variable). A key is never ignored silently. Nothing is reloaded while the server runs except the TLS certificate and key (`SIGHUP`, section 7); to change a limit, restart.

The defaults and the allowed range of every limit are in `docs/REQUIREMENTS.md` section 4 (checked against the code in `docs/LIMITS_AUDIT.md`); there is a hard ceiling on every one of them and no value means "unlimited" except the per-address limits (section 1.2). The keys, by what they do:

| Area | Keys |
|---|---|
| Listening | `listen_host`, `listen_port`, `threads` (event loops: start at the number of cores you give it) |
| Who may connect | `allow_origin` (repeatable), `allow_missing_origin`, `trusted_proxy` (repeatable), `client_ip_header` |
| Limits | `max_connections`, `max_connections_per_ip`, `max_new_connections_per_ip_per_second`, `max_message_size`, `max_frame_payload`, `max_fragments`, `max_data_frames_per_second`, `max_pending_send_bytes`, `event_queue_events`, `event_queue_bytes`, `shutdown_grace_ms` |
| Authenticating upgrades | `handshake_hook`, `handshake_forward_header` (repeatable), `handshake_decision_timeout_ms`, `max_pending_handshakes` |
| TLS | `tls_cert`, `tls_key`, `tls_sni` (repeatable) |
| Compression | `permessage_deflate`, `deflate_server_max_window_bits`, `deflate_server_no_context_takeover`, `deflate_level`, `deflate_min_size`, `deflate_max_ratio` (section 1.1) |
| Observability | `admin_host`, `admin_port` (section 3.1), `log_level`, `log_peer` (section 2) |
| Rooms and publishing | `broker`, `broker_max_topics`, `broker_max_subscribers_per_topic`, `broker_max_topics_per_connection`, `broker_max_subscriptions`, `broker_max_payload_bytes`, `publish_host`, `publish_port`, `publish_token_file` (sections 3.1.1, 3.2) |
| `wsrvd` only | `ipc_socket` or `ipc_port` and `ipc_host` (one of the two transports, section 7), `ipc_token_file`, `ipc_max_frame`, `ipc_hello_timeout_ms`, `ipc_stall_ms` (section 7) |

The IPC and publish tokens are never keys: they would show in `ps` and `/proc`, so only a file (mode 0600, yours) can carry them.

### 1.1 Compression (permessage-deflate, RFC 7692, ADR-0020)

Off by default. Switch it on with `permessage_deflate = true` (library and `wsrvd`; `wsrvd --permessage-deflate=true`). The handler or PHP code still sees whole, inflated messages and sends plain ones; compression happens inside the server. A build without zlib (`WSRV_ENABLE_DEFLATE=OFF`) refuses to start with the switch on instead of silently serving uncompressed.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `permessage_deflate` | `false` | bool | Offer the extension to clients that ask for it |
| `deflate_server_max_window_bits` | `12` | 9..15 | Window of the server's compressor (memory against ratio). 8 is refused: zlib cannot compress with it |
| `deflate_server_no_context_takeover` | `true` | bool | Reset the compressor after every message: less state, slightly worse ratio |
| `deflate_level` | `6` | 1..9 | zlib level: CPU against ratio |
| `deflate_min_size` | `128` | bytes | Shorter outgoing messages are sent uncompressed (always allowed) |
| `deflate_max_ratio` | `100` | 1..1000 | An incoming message may inflate to at most this many times its compressed size (plus 4 KiB), else the connection is closed with 1009 |

The tuning keys only matter while the switch is on, in any order; a value outside its range makes startup fail with the key's reason.

**Memory per connection.** zlib's own formula (zconf.h): deflate takes `2^(windowBits+2) + 2^(memLevel+9)` bytes plus a few KiB of bookkeeping; inflate takes `2^15` bytes plus a few KiB (our inflater always uses a 15-bit window, which decodes whatever the client's window is). Nothing is allocated until the first compressed message in that direction, and the memory stays with the connection until it closes (the compressor is reset between messages, not freed). A 16 KiB buffer for inflated output is added by the first compressed message received. The send path keeps no buffer between messages.

| Server window | deflate, memLevel 6 (default) | deflate, memLevel 8 (zlib's default) |
|---|---|---|
| 9 | 34 KiB | 130 KiB |
| 10 | 36 KiB | 132 KiB |
| 11 | 40 KiB | 136 KiB |
| **12** (default) | **48 KiB** | 144 KiB |
| 13 | 64 KiB | 160 KiB |
| 14 | 96 KiB | 192 KiB |
| 15 | 160 KiB | 256 KiB |

(`memLevel` is not a configuration key: it is 6.) Add 32 KiB for the inflater and 16 KiB for its output buffer when the connection also receives compressed messages. So a connection that sends and receives compressed messages costs about **96 KiB + a few KiB at the defaults** and about 300 KiB at window 15 with memLevel 8. With `max_connections = 10000` and every client compressing, budget about 1 GB at the defaults; this is the number to put into `MemoryMax` and capacity planning, on top of the buffers in section 5.

**Work per read.** The bomb limit is a ratio, so one 64 KiB read (the per-turn read budget) can legitimately inflate to about 6 MB at ratio 100 before the loop turns to the next connection. Real JSON compresses 5 to 20 times: if your messages do, lower `deflate_max_ratio` (for example 30) to shrink the worst case a hostile client can cause per read; the message size limit (`max_message_size`) applies to the inflated bytes regardless.

**Security.** Compressing text that mixes a secret with attacker-influenced data in one message lets an observer of message sizes guess the secret (CRIME/BREACH class): leave compression off for such applications. There is no per-message opt-out yet. Messages sent through the streaming writer (`MessageWriter`) are not compressed.

### 1.2 Per-address limits (ADR-0021)

Two limits keep one client address from taking the whole server: `max_connections_per_ip` (default 100, live connections) and `max_new_connections_per_ip_per_second` (default 50, a token bucket: a burst of one second's worth, then the rate). `0` switches a limit off. In the embedded/`wsrvd` text configuration they are the keys of the same names (`--max-connections-per-ip=N`, `--max-new-connections-per-ip-per-second=N`).

- **Who is "one address":** an IPv4 address; an IPv4-mapped IPv6 address counts as the IPv4 address it maps; any other IPv6 address counts as its **/64** (a subscriber owns at least a /64, so single addresses would let one machine rotate through 2^64 of them). The port is ignored.
- **What the client sees:** `HTTP/1.1 429 Too Many Requests` with `Retry-After: 1` and a closed connection. A client that already sent its request may see a reset instead: the answer is best effort, the close is the guarantee. The global cap (`max_connections`) keeps its silent close.
- **Behind a reverse proxy** every user would share the proxy's address, so tell the server which peers are your proxies (section 1.2.1) and the limits then count real clients. **Behind a NAT or a PROXY-protocol-only load balancer** (no header) every user is still one address: raise the limits or set them to `0` and rate-limit at the balancer (the PROXY protocol is not implemented yet; ADR-0029 section 6).
- **Cost:** a table of at most `2 * max_connections` entries (about 40 bytes plus the map node each), one mutex taken twice per connection life. An idle server holds no per-address state. When the table is full a new address is refused (fail closed), after one sweep of entries that carry no information.

#### 1.2.1 The real client address behind a reverse proxy (ADR-0029)

```ini
trusted_proxy = 10.0.0.0/8      # repeatable: an address or a CIDR, at most 64; empty (default) = off
trusted_proxy = 192.168.1.5
client_ip_header = X-Forwarded-For   # default; any other name is read as one single address
```

- **Nothing is believed by default.** The header is read only when the TCP peer is inside `trusted_proxy`; from any other peer it is ignored, so a client that sends `X-Forwarded-For` straight to `wsrvd` gains nothing. List only machines you run.
- **Which entry is the client:** for `X-Forwarded-For` (a list every proxy appends to) the **rightmost entry that is not itself a trusted proxy**; what lies left of it is controlled by the client and never used. For `X-Real-IP`, `CF-Connecting-IP`, `True-Client-IP` and the like, the single value. Your proxy must set or overwrite the header (nginx: `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;` for a list, `proxy_set_header X-Real-IP $remote_addr;` for one value).
- **Fail safe:** a missing header, an entry that is not an address, more than 32 entries or more than 1 KiB all leave the proxy's own address in place, so the limits only get stricter.
- **What changes:** a connection from a trusted proxy takes no per-IP slot at accept (the proxy would exhaust its own budget) but still counts against `max_connections`. Once its upgrade request is valid, the client address is admitted through the per-IP limits (429 or 503 on refusal, like any handshake refusal) and the slot comes back when the connection ends. The application's `open` event, the handshake hook and the logs then show the client, with port 0.
- **Not yet:** PROXY protocol v1/v2 and the `Forwarded` (RFC 7239) header.

### 1.2.2 Application sends waiting for a loop (ADR-0032)

An application (the C API, the PHP worker through `wsrvd`) sends from its own thread: each `send_text` / `send_binary` is copied and queued to the loop that owns the connection. The queue is bounded in count, and since ADR-0032 also in bytes: `max_pending_send_bytes` (per loop, default 64 MiB, 1 B to 1 GiB, no unlimited value; `WSRVD_MAX_PENDING_SEND_BYTES` in a container). When the budget is used up a send returns `BufferFull` (C: `WSRV_ERR_BACKPRESSURE`, a PHP worker sees status `BACKPRESSURE`): wait and retry. A single message larger than the whole budget is refused for good with `InvalidArgument`, so keep the budget above your largest message. Worst case memory is `threads x budget`. Broadcasts and control commands are not charged (one shared buffer, and they must always get through).

### 1.3 Frame, fragment and frame-rate limits (ADR-0016 A1)

Three per-connection limits protect the server from a single peer, and each has a text key in the embedded and `wsrvd` configuration. They exist because some legitimate clients need more than the defaults (a trusted batch uploader; the Autobahn conformance run), not because the defaults are wrong for the web.

| Key | Default | Accepted | What raising it costs |
|-----|---------|----------|-----------------------|
| `max_frame_payload` | 1 MiB | 1 to 64 MiB | the largest buffer one connection can make the server hold for one frame; raise it with `max_message_size` |
| `max_fragments` | 1024 | 1 to 16 777 216 | bookkeeping per fragment, not memory; a peer can make the server do more work per message |
| `max_data_frames_per_second` | 10 000 | 1 to 10 000 000 | the flood guard: a peer may send that many frames per second before it is closed with 1008 |

None of them has an "off" value; a refused value changes nothing and `wsrv_config_set` reports `WSRV_ERR_INVALID_CONFIG`. Whoever sets `max_message_size` above the event-queue size must also raise `event_queue_bytes`: the host receives messages whole, so a queue smaller than one message cannot deliver it.

**The event queue and the connection ceiling.** Events for the application (a connection opened, a message arrived, a connection closed) wait in a bounded queue between the I/O threads and the application. Every live connection holds one slot of it for its own `Close` event, so the queue must be larger than the number of connections: with `event_queue_events` unset (the default) it is `max_connections + 4096` and `event_queue_bytes` is `max_connections * 123 + 8 MiB`, so the default is consistent for any `max_connections`. If you set `event_queue_events` yourself, it is used as given, and a value below `max_connections` caps the server at that value (the connection beyond it is accepted, upgraded and closed with the overload close code 1013). Before 1.0 the queue was a fixed 4096 and capped every server at 4095 connections.

## 2. Logging

`wsrvd` and the embedded server (PHP FFI) write **one line per event to stderr**. Nothing else is written there while the server runs, and nothing is ever written to a file by wsrv: systemd (`journalctl -u wsrvd`), Docker (`docker logs`) and Kubernetes (`kubectl logs`) already collect, rotate and ship stderr. Decision record: ADR-0037.

```
ts=2026-11-26T12:00:00.123Z level=info event=listening msg="server: websocket listening on port 8080"
ts=2026-11-26T12:00:07.884Z level=warn event=origin_refused conn=17 msg="handshake refused: the Origin header is not allowed"
ts=2026-11-26T12:00:09.001Z level=warn event=ip_limit suppressed=412 msg="server: per-IP limit reached, refusing clients"
```

### 2.1 The line

| Field | Meaning |
|---|---|
| `ts` | RFC 3339, UTC, milliseconds |
| `level` | `error`, `warn`, `info` or `debug` |
| `event` | a stable name (section 2.3): match on this in alerts, not on the text |
| `conn` | the connection number, to follow one connection through its lines; absent when the line is not about one (the same number the application sees as the connection id) |
| `peer` | the client address, **only with `log_peer`** (section 2.2) |
| `suppressed` | present when the line stands for more than one: how many of this event were held back since the last line (section 2.4) |
| `dropped` | present when earlier lines could not be written (section 2.4) |
| `msg` | a short sentence for people, always in double quotes |

Field names and event names are stable within 1.x (new ones may be added; a parser must ignore what it does not know). The `msg` text is not: do not match on it.

One event is exactly one line, whatever a client sent: `msg` is quoted, `\` `"` and every control character and byte above 0x7e are escaped (`\n`, `\xNN`), so a hostile client cannot forge a line or smuggle a terminal escape. A line is at most 512 bytes. In practice the message is fixed text and carries nothing the client sent.

### 2.2 Settings

| Key | Default | |
|---|---|---|
| `log_level` | `info` | `error`, `warn`, `info` or `debug`. A value below the level costs one comparison. Any other word is a startup error (a typo does not silently mean `info`). |
| `log_peer` | `false` | Put the client's address in the lines about a connection. An IP address is personal data (GDPR), so it is off unless you decide you need it: turn it on to find out who is hitting a limit, leave it off to keep the logs free of personal data. |

Flags, `--config` lines and environment: `--log-level=warn`, `log_level = warn`, `WSRVD_LOG_LEVEL=warn`, `WSRVD_LOG_PEER=true`. In the embedded server they are ordinary keys (`log_level`, `log_peer`).

Never logged at any level: tokens and credentials, cookies and other headers, message payloads, anything the client chose (an Origin, a path, a close reason). The line says that the Origin was refused, not which one it was.

### 2.3 Events

| Event | Level | Meaning and what to do |
|---|---|---|
| `listening` | info | the WebSocket port is open (`msg` has the port) |
| `publish_listening`, `admin_listening` | info | the publish / admin endpoint is open (`msg` has the address) |
| `publish_exposed`, `admin_exposed` | warn | that endpoint is not on loopback: keep it on a private network (sections 3.1 and 3.1.1) |
| `ipc_ready` | info | `wsrvd` is ready for the application, on its Unix socket or (ADR-0038) on its TCP address, with the port actually bound |
| `ipc_no_token` | warn | no `ipc_token_file`: only the peer uid check protects the socket (never said over TCP, where a token is required) |
| `ipc_plaintext` | warn | the TCP IPC link is bound to an address that is not loopback; it is not encrypted (ADR-0038). Expected in a container whose port you publish on the host's loopback; a warning anywhere else |
| `ipc_keepalive` | warn | TCP keepalive could not be set on the application's socket: a host that vanishes may keep the application slot for a long time |
| `ipc_linger_full` | debug | over TCP, more than 32 refused connections were waiting to close at once; the extra ones were closed immediately (a flood of refused connections; the status was still sent) |
| `app_waiting` | debug | over TCP, a connection was admitted to the waiting room (at most 8 wait for their HELLO at once, ADR-0038 section 5) |
| `app_replaced` | warn | over TCP, the waiting room was full (8 connections that had not said HELLO) and the oldest was evicted and told `BACKPRESSURE`. Occasional: a probe or a port scan. Repeated: connections arrive faster than your worker's HELLO, so it can be refused; check what else reaches the IPC port and who may |
| `app_connected`, `app_disconnected` | info (warn if cut off for not reading) | the application's link came and went: connections it knew are closed with 1012 after `shutdown_grace_ms` unless a new one connects |
| `app_rejected` | warn | an application was refused: another user, a second one, a wrong token or version, no HELLO in time. Many of these: a misconfigured worker (compare tokens and the socket path) |
| `tls_reloaded` | info | SIGHUP picked up a new certificate |
| `tls_reload_skipped` | info | SIGHUP, but TLS is off |
| `tls_reload_failed`, `tls_config` | error | the certificate or key cannot be used; `tls_config` lines name the file and the reason. The old certificate stays in use |
| `conn_limit`, `ip_limit`, `loop_overloaded` | warn | a limit refused a client (section 1.2 and `max_connections`) |
| `origin_refused` | warn | the Origin header is not in `allow_origin`. A wrong list shows up here at once; scanners show up as a rate-limited stream |
| `handshake_refused` | info | the upgrade was refused for another reason (malformed, too large). Any client can cause it, so it is deliberately not a warning |
| `handshake_undecided` | warn | the application could not be asked about an upgrade (its queue is full or it is gone), so the client was refused with 503. This one is about your server, not the client |
| `handshake_timeout`, `handshake_decision_timeout` | info, warn | the upgrade was not completed in time (slow clients; the application did not answer the handshake hook) |
| `decision_queue_full` | warn | too many upgrades waiting for the application's decision |
| `slow_consumer`, `peer_stalled` | warn | a connection was closed for not reading what was sent to it |
| `protocol_error` | info | the peer broke RFC 6455 |
| `handler_misuse` | warn | the application's handler did something not allowed (an unfinished message, a close code that cannot be sent) |
| `publish_rejected` | warn | the publish endpoint refused a request; `msg` has the status (401: wrong token) |
| `conn_open`, `conn_close` | debug | each connection, and its close code |
| `descriptor_limit` | error | the start failed: `ulimit -n` is below what `max_connections` needs |
| `descriptor_limit_raised` | info | the soft descriptor limit was raised to what is needed |
| `start_failed`, `internal_error`, `out_of_memory`, `poller_failure`, `timer_failure`, `transport_failed`, `handler_exception`, `no_handler`, `shutdown_failed` | error | the server could not do its job for this connection or loop; a steady stream is a bug or an exhausted machine: collect the lines and report them |
| `shutdown` | info | the stop signal arrived |

### 2.4 It never stalls the server, and it cannot be flooded

- **Per event, a burst then a trickle.** Each event may write 10 lines at once, then one a second. What was held back is counted and the next line of that event says `suppressed=N`. A client causing a million refused handshakes costs about one line a second. This holds at every level, `debug` too: on a busy server `conn_open` and `conn_close` are a sample, and `suppressed=` tells you how many there were (the connection counters on `/metrics` are exact).
- **A stuck destination costs lines, never time.** Each line goes out in one non-blocking write after a check that the descriptor can take it. If stderr is a pipe that nobody reads (a hung log driver), the line is dropped and counted, the I/O loop never waits, and the next line that gets out says `dropped=N`. The counters on `/metrics` are the record that does not depend on the log.
- **No log thread**, no file, no rotation: stderr is the interface.

## 3. Metrics (Prometheus text exposition on a separate admin listener, loopback by default)

ADR-0023. Counters live in one bank per loop (relaxed atomics, no lock) and are summed when read; every label value is a compile-time constant, so peer data can never create a time series. A scrape is a snapshot taken counter by counter (not atomic across counters). Embedders read the page with `Server::metrics_text()`; the admin listener that serves it over HTTP is the next slice (section 3.1).

| Metric | Type | Status |
|---|---|---|
| `wsrv_connections_current` | gauge | done |
| `wsrv_connections_total{result}` | counter | done: `accepted`, `rejected_global_cap` (also a loop too overloaded to take a client), `rejected_ip_connections`, `rejected_ip_rate`, `rejected_ip_table`, `handshake_failed` |
| `wsrv_messages_total{direction,type}` | counter | done: `in`/`out` x `text`/`binary` (a streamed message counts when it starts) |
| `wsrv_bytes_total{direction}` | counter | done: bytes at the socket (ciphertext under TLS); `out` counts bytes the transport accepted |
| `wsrv_close_total{class}` | counter | done: `normal` (1000, 1001), `protocol_error` (1002, 1003, 1007, 1009, 1010), `policy` (1008), `abnormal` (no Close frame: 1005, 1006, 1015), `other` (1011-1014 and application codes) |
| `wsrv_slow_consumer_drops_total` | counter | done: slow-consumer close (1008) plus write-stall drops |
| `wsrv_accept_errors_total{errno}` | counter | done: `emfile` (out of descriptors, the reserve-fd trick ran), `other` |
| `wsrv_tls_handshake_failures_total` | counter | done: TLS failures before the upgrade |
| `wsrv_broker_publishes_total` | counter | done: validated publishes on the pub/sub broker (section 3.2); only once the application attaches the broker with `Broker::use_metrics(server.shared_metrics())` |
| `wsrv_broker_deliveries_total{result}` | counter | done: `accepted` (the subscriber's loop took the message for delivery), `refused` (its task queue was full, out of memory, or the server is gone); every subscriber of a publish is one or the other |
| `wsrv_handshake_seconds` | histogram | not in 1.0, planned (ADR-0023 section 5) |
| `wsrv_loop_lag_seconds{loop}` | histogram (detects blocking handlers) | not in 1.0, planned |
| `wsrv_outbound_queue_bytes` | histogram | not in 1.0, planned |
| `wsrv_config_reload_total{result}` | counter | not in 1.0, planned (`wsrvd` reload) |

### 3.1 Admin endpoint
Off unless configured: embedded/`wsrvd` keys `admin_port` (setting it switches the listener on; 0 lets the kernel choose) and `admin_host` (default `127.0.0.1`); `wsrvd --admin-port=9100`. Plain HTTP, **no authentication**: keep it on loopback or behind a network policy (a non-loopback bind logs a warning at start). It serves exactly three pages:

| Request | Answer |
|---|---|
| `GET /metrics` | `200`, the Prometheus text of section 3 |
| `GET /healthz` | `200 ok` while loop 0 answers (liveness; a stalled loop 0 stalls this too, which is the signal) |
| `GET /readyz` | `200 ready` while the server accepts; `503 not ready` once a shutdown has begun (take the instance out of the load balancer before the grace period ends) |
| any other target | `404` (exact match: no query string, no case folding); other methods `405`, a malformed request `400` |

Bounds (ADR-0023, SECURITY_DESIGN T21): the admin listener runs on loop 0 (no extra thread); at most 8 clients at once (more are closed at once), a request head of at most 2 KiB, 5 s to send it, one request per connection, one non-blocking send for the answer (a client that cannot take the whole page in one call is dropped, so a page is never silently truncated). Prometheus example: `scrape_configs: [{job_name: wsrv, static_configs: [{targets: ["127.0.0.1:9100"]}]}]`.

### 3.1.1 Publish endpoint (HTTP publish from any process)
ADR-0028, SECURITY_DESIGN T22. Lets a PHP-FPM request, a queue job or a cron script push a message to the browsers in a room with one HTTP call. Off unless `publish_port` is set: `wsrvd --broker=true --publish-port=9200 --publish-token-file=/run/secrets/wsrv_publish --publish-host=127.0.0.1`. It needs the broker (rooms are what it publishes to) and a token file (a regular file, owned by the service user, mode 0600; one trailing newline is ignored). **From 1.1 the token must be one `wsrvd --generate-token=FILE` made** (ADR-0039): a token somebody chose can be guessed however long it is, so any other stops the start with a message that names the fix, on loopback too. **Upgrading from 1.0** with a token you made yourself (`openssl rand`, `head -c 32 /dev/urandom`): make a new one (`mv publish.token publish.token.old && wsrvd --generate-token=publish.token`, as the service user), restart `wsrvd`, and give the new file to every publisher; the old token stops working at the restart. The token is never a key or a flag: it would show in `ps` and in config repositories.

```
POST /publish?topic=room.7[&except=<connection id>][&binary=1]
Authorization: Bearer <token>
Content-Length: <n>

<the message bytes>
```
| Answer | Meaning |
|---|---|
| `200` | `subscribers=<n> accepted=<n> refused=<n>`: how many were in the room, took the message, or had a full queue (a slow subscriber is isolated, as in ADR-0024) |
| `400` | bad request: repeated or non-decimal `Content-Length`, bad or unknown query parameter (no `%`-escapes), a topic the broker does not accept, bytes after the body |
| `401` | no, wrong or malformed token (always the same words) |
| `404` / `405` | not exactly `/publish` / not `POST` |
| `411` / `501` / `417` | no `Content-Length` / any `Transfer-Encoding` (chunked included) / any `Expect` |
| `413` | `Content-Length` over `broker_max_payload_bytes` (checked only after the token) |
| `415` | a text message that is not UTF-8 (send `binary=1` for other bytes) |
| `429` | there were subscribers and every delivery was refused: the loops' queues are full, retry later |
| `503` | the server is shutting down, or out of memory |

No JSON: the body is the message, so you send what you already built. Bounds: head at most 4 KiB, 5 s for head and body together, at most 16 clients at once, one request per connection. **Plain HTTP:** on a non-loopback `publish_host` the daemon logs a warning, because the token travels in clear text. In containers put `wsrvd` and the application on an internal network and use `expose:`, not `ports:` (see `examples/docker`). Counters: `wsrv_publish_http_total{status}` on the admin page. `curl`:
`curl -sS -X POST "http://127.0.0.1:9200/publish?topic=room.7" -H "Authorization: Bearer $(cat token)" --data-binary 'hello'`.

### 3.2 Pub/sub broker (C++ API)
ADR-0024. `Broker` is an object of the application's composition root: handlers call `subscribe`, `unsubscribe`, `publish_text` and `publish_binary` from any thread; a room is a topic. Hold a `BrokerMembership` in each handler so its subscriptions end with its connection. Every bound is a `BrokerConfig` field and every overflow is a refusal that changes nothing:

| Bound | Default | When exceeded |
|---|---|---|
| Distinct topics | 1024 | `subscribe` returns `BufferFull` |
| Subscribers per topic | 10 000 | `BufferFull` |
| Topics per connection | 64 | `BufferFull` |
| Subscriptions in total | 100 000 | `BufferFull` (what stops a handler that forgets to leave) |
| Topic name | 1 to 128 bytes of `[A-Za-z0-9._:/@-]` | `InvalidArgument` |
| Payload per publish | 64 KiB | `InvalidArgument` |

Two cautions (ADR-0024 amendments). **Compression:** with permessage-deflate negotiated every subscriber compresses its own copy of each broadcast, so a publish to thousands of subscribers costs thousands of zlib runs on their loops; for a broker deployment leave compression off or lower `max_payload_bytes`. **Shared bounds:** the limits above are global; they cannot tell one user from another, so an application that serves strangers must apply its own quota per account or address before calling `subscribe`. Broadcasts use a reserved share of each loop's task queue: they are refused once it is three quarters full, so new connections, handshake decisions, `close` and shutdown still get through while a publish flood is being shed.

What to watch: `wsrv_broker_deliveries_total{result="refused"}` rising means a loop's task queue is full (the publish rate outruns a loop); a `PublishReport` says the same per call. A loop accepts a broadcast in chunks of 256 recipients, one queue slot per chunk (ADR-0034), so the size of a room alone never fills it: a loop takes a publish to hundreds of thousands of recipients at once, and refuses a chunk only when publishes arrive faster than the loop drains them (a refused chunk counts all its recipients as refused). Delivery is "accepted for delivery", never acknowledged; a subscriber that cannot keep up is closed or dropped by its own connection's slow-consumer policy (`wsrv_slow_consumer_drops_total`), and the other subscribers of the same publish are unaffected. The C ABI and the PHP `Client::publish()` are not available yet (ADR-0024 section 7).

## 4. Lifecycle

- **Start:** validate config → check `RLIMIT_NOFILE` ≥ `max_connections + 64 + 4 × threads` (ADR-0022: a lower soft limit is raised to exactly that when the hard limit allows, otherwise the start fails with `FdLimit` and a log line naming needed, soft and hard) → bind → (optionally drop privileges) → start loops → log `listening`.
- **Embedded hosts (C API, PHP):** the same check runs in `wsrv_server_start` and a raise changes the soft limit of the *host* process (only upwards, only to what the configuration needs). A host that must keep a small limit lowers `max_connections`.
- **Reload (SIGHUP):** `wsrvd` re-reads the TLS certificate and key only (section 7); a failed reload keeps the certificate in use and logs why. Everything else needs a restart.
- **Graceful stop (SIGTERM/SIGINT):** stop accepting (readyz fails) → send Close 1001 to all → wait up to `shutdown_grace` (default 10 s) → force close remaining → join threads → exit 0. A second signal is not special (see "Signals and exit codes"): use `SIGKILL` to stop at once.
- **Crash policy:** invariant violation ⇒ abort with core dump (supervisor restarts); never continue in an unknown state.

## 5. Deployment hardening

- Run as non-root; bind low ports via `CAP_NET_BIND_SERVICE` or reverse proxy.
- systemd unit: `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, `PrivateDevices=yes`, `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX`, `SystemCallFilter=@system-service`, `LimitNOFILE=…`, `MemoryMax=…`, `Restart=on-failure`.
- Container: the official image runs as uid 10001 with a built-in `HEALTHCHECK` and only the libraries the daemon links against. Run it with a read-only root filesystem and every capability dropped, as `deploy/kubernetes/wsrv.yaml` does.
- Kernel tuning (documented, not applied by the server): `net.core.somaxconn`, `net.ipv4.tcp_max_syn_backlog`, `tcp_syncookies=1`, `fs.file-max`, ephemeral port range, `tcp_keepalive_*`.
- Behind a reverse proxy: configure `trusted_proxy`, ensure proxy timeouts > heartbeat, forward `Upgrade/Connection`, pass the real client IP in `X-Forwarded-For` (or another single-address header) and list the proxies in `trusted_proxy` (section 1.2.1); the PROXY protocol is not implemented yet.
- Capacity planning: memory/connection × max_connections + buffer pool headroom; measured in `docs/PERFORMANCE.md` and `docs/BENCHMARKS.md`.

## 6. Runbook

| Symptom | Check | Likely cause |
|---|---|---|
| Start fails with `FdLimit` | log line "needs N descriptors ... soft / hard" | `ulimit -Hn` or systemd `LimitNOFILE=` below `max_connections` + reserve: raise it or lower `max_connections` |
| Accept errors `EMFILE` | `wsrv_accept_errors_total`, `ulimit -n` | descriptor leak, or other code in an embedding process using the headroom |
| Many refusals | `wsrv_connections_total{result="rejected_*"}` | global cap, per-IP limits (section 1.2) or an address-spraying flood (`rejected_ip_table`) |
| High `loop_lag` | handler profile | blocking user code |
| Rising outbound queue / slow-consumer drops | per-conn histogram | slow clients, broadcast too large |
| Real users get 429 | client address behind one proxy/NAT? | per-address limits (section 1.2): raise or set to 0, limit at the proxy |
| Many `handshake_failed` | log sample, Origin list | wrong origin config / scanners |
| Memory growth | pool metrics, deflate on? | zlib state per connection (section 1.1), leak (run soak with heaptrack) |
| Clients dropping every N seconds | proxy idle timeout vs heartbeat | proxy timeout < ping interval |

## 7. Running `wsrvd` (the sidecar)

Every setting below is also an environment variable, `WSRVD_` plus the key in upper case (`WSRVD_LISTEN_PORT`); the environment is the base layer and `--config` and flags override it. `wsrvd --healthcheck` probes a running daemon (exit 0 = healthy). An unknown `WSRVD_*` variable stops the start. Tokens are files, never variables (ADR-0030).

What the daemon does (ADR-0017, IPC protocol in `docs/IPC_PROTOCOL.md`). Metrics and the admin listener are in section 3.

### Start-up
```sh
mkdir -m 700 /run/wsrv                       # private directory, owned by the service user (or root)
wsrvd --generate-token=/etc/wsrv/ipc.token     # 256 random bits, mode 0600, never overwrites; chown it to the service user
wsrvd --ipc-socket=/run/wsrv/ipc.sock --ipc-token-file=/etc/wsrv/ipc.token \
      --listen-host=0.0.0.0 --listen-port=8080 --allow-origin=https://app.example.com
```
- **TCP instead of the Unix socket** (ADR-0038): `--ipc-port=9300 --ipc-token-file=FILE` (and `--ipc-host=ADDR`, default `127.0.0.1`) for a worker on another host, or on macOS or Windows against a daemon in Docker. Set exactly one of `ipc_socket` and `ipc_port`. The token is **mandatory** and must be one `wsrvd --generate-token=FILE` made (`wsrv1_` and 64 hex digits and a checksum; there is no peer uid to check over TCP, so the token is the only authentication, and a token somebody chose can be guessed however long, so the daemon refuses it at start and says what to run), and the link is **plaintext**: keep it on loopback, a private network, a VPN or an SSH tunnel. In a container, bind `0.0.0.0` inside and publish the port on the host's loopback only (`-p 127.0.0.1:9300:9300`); the daemon then logs the `ipc_plaintext` warning, which is expected there. The official image has a built-in default socket path that applies only when neither key is set, so `-e WSRVD_IPC_PORT=9300` alone selects TCP. The container health check does not probe the TCP port. `ipc_port=0` lets the kernel choose; `ipc_ready` logs the port.
- The socket directory must not be group- or world-writable and must belong to the service user or root. The daemon creates the socket with mode `0600` and refuses to follow a symlink at that path.
- The token file must be mode `0600` and owned by the service user. There is **no** command-line or config-file value for the token (it would show in `ps` and `/proc`). Without a token only the peer-uid check protects the socket, and the daemon warns at start-up.
- **`wsrvd --generate-token=FILE`** (1.1) writes a new random IPC token to `FILE` (`wsrv1_`, 64 hex digits of 32 bytes from the kernel's random source, `_`, a CRC-32; mode `0600`; one newline) and exits. It refuses an existing file (a token is a deployment's secret: delete the old one on purpose to replace it), never follows a symlink, never prints the token, and takes no other argument. It is the only way to make a token the TCP transport accepts, and a good one for a Unix socket too. In the image: `docker run --rm -v wsrv-secrets:/run/wsrv ghcr.io/dimathjayasinghe/wsrvd --generate-token=/run/wsrv/ipc.token` (the volume must be writable by uid 10001).
- `--config=FILE` reads `key = value` lines; later arguments override earlier ones. Keys: `ipc_socket`, `ipc_token_file`, `ipc_max_frame`, `ipc_hello_timeout_ms`, `ipc_stall_ms`, the handshake-hook keys below, plus every embedded-server key (`listen_host`, `listen_port`, `threads`, `tls_cert`, `tls_key`, `allow_origin`, `allow_missing_origin`, `max_message_size`, `max_connections`, `shutdown_grace_ms`, ...).
- **wss:// (TLS)**: add `--tls-cert=/etc/wsrv/cert.pem --tls-key=/etc/wsrv/key.pem` (PEM; the certificate file holds the server certificate first, then any intermediates). Setting either makes the port TLS-only (TLS 1.2 and 1.3, forward-secret AEAD suites only, ALPN `http/1.1`); plain `ws://` is not served on that port. The key file must not be readable by "others" (mode `0640` or `0600`) and must have no passphrase; an expired certificate, a key that does not match, a missing file, only one of the two options, or a build without TLS (`WSRV_ENABLE_TLS=OFF`) all stop the start-up with the reason on stderr: the daemon never falls back to plain text. Details and the threat model: ADR-0019.
- **Several certificates (SNI)**: repeat `--tls-sni=NAME=CERT_FILE:KEY_FILE` (at most 64), for example `--tls-sni=app.example.com=/etc/wsrv/app.pem:/etc/wsrv/app.key --tls-sni='*.example.org=/etc/wsrv/org.pem:/etc/wsrv/org.key'`. The client names the host it wants in its TLS hello; matching ignores case, an exact name beats a wildcard, and `*.example.org` covers exactly one label (`a.example.org`, not `example.org` or `a.b.example.org`; `*.org` is refused). A client that sends no name (or an IP address, or an unknown name) receives the `--tls-cert` certificate, so that one is required and acts as the default. Every certificate is held to the same rules as the default (valid now, key matches, key file not open to others), and all of them are re-read on `SIGHUP`; if any one fails, nothing changes. Paths must not contain `:`.
- The daemon logs `event=ipc_ready` and `event=listening` on stderr when it is ready (section 2). Mistakes in the command line itself are printed plainly with the usage text, before any logger exists.

### Signals and exit codes
- `SIGTERM` / `SIGINT`: graceful stop. The IPC server closes first (the socket file is removed), then the WebSocket server sends Close 1001 to every connection, waits up to `shutdown_grace_ms`, closes the rest and joins its threads. Exit code 0.
- A **second** signal during the graceful stop is not treated specially today (it does not force an immediate exit); use `SIGKILL` if a stop hangs. A stale socket file left by `SIGKILL` is recovered by the next start (it probes the socket and removes it only if nothing answers and it is ours).
- `wsrvd` installs its signal handling before it creates any other thread (signals are read from a descriptor), so every thread it starts inherits the mask.
- Exit code 1: bad configuration, unusable socket path or token file, bind failure, or an event-loop error. The reason is printed on stderr.
- `SIGHUP`: re-read the TLS certificate and key (`tls_cert`, `tls_key`) and use them for new connections; open connections keep the old one. Nothing else is reloaded yet. The daemon logs `event=tls_reloaded`, or `event=tls_config` lines naming the file and the reason followed by `event=tls_reload_failed` (the old certificate stays), or `event=tls_reload_skipped` when TLS is off. A failed reload never stops the server. Typical use: after the certificate renewal tool (certbot, acme.sh) writes the new files, run `kill -HUP $(pidof wsrvd)` (for systemd: `ExecReload=/bin/kill -HUP $MAINPID`).

### Fork and process rules
- `wsrvd` never forks. Run it under a supervisor (systemd, supervisord, a container runtime).
- The embedded library (`libwsrv.so`) refuses calls from a forked child of the process that created the server (`WSRV_ERR_WRONG_PROCESS`): threads do not survive `fork()`. Create the server after the fork.
- An application connects with one process per daemon. Do not share the IPC connection across a fork.

### Authenticating connections (the handshake hook)
By default `wsrvd` accepts every upgrade that passes its own checks (Origin, version, syntax). To let your application decide, from the cookie or token the browser sent, turn the hook on:

```
handshake_hook = true
handshake_forward_header = Cookie        # repeatable; Cookie/Authorization are sent only if listed
handshake_decision_timeout_ms = 3000     # how long the application has to answer (default 3000)
max_pending_handshakes = 256             # upgrades waiting at once (default 256); more get 503
```

The application receives each upgrade as a Handshake event (`docs/IPC_PROTOCOL.md` section 9) and answers accept or reject. Things to know:
- **It fails closed.** No answer in time, no application connected, an application that disconnects mid-question, or shutdown: the browser gets `503 Retry-After: 5`, never a connection.
- Applications must speak IPC minor 2; the daemon refuses an older one rather than leave browsers waiting. The PHP client in `bindings/php` does.
- Anything you list in `handshake_forward_header` passes through the application process. The default forwards nothing.
- `handshake_decision_timeout_ms` is the longest a browser waits for its `101`. Keep it short; a slow login check belongs in the application's own cache.
- Without an application the daemon no longer "holds" browsers behind backpressure (see the table below): with the hook on, they are refused at once.

### What happens when things go wrong
| Situation | Behaviour |
|---|---|
| No application connected | New WebSocket clients complete the handshake and wait (with `handshake_hook` on they get `503` at once instead); events queue (bounded) and the queue's own limits pause the clients. The first application receives everything in order. |
| Application disconnects | (With `handshake_hook`, upgrades it had not answered get `503` at once.) Connections it knew are closed with 1012 after `shutdown_grace_ms`; its later events for them are discarded; connections it never saw wait for the next application. |
| Application stops reading for `ipc_stall_ms` | The IPC link is closed and its connections are closed with 1001 at once. |
| Application sends garbage | The IPC link is closed with no reply. |
| Second application connects | Refused with `STATUS(0, BACKPRESSURE)`; the first is unaffected. |
| Daemon restarts | Browsers see the close or a dropped connection and must reconnect; messages in flight are lost (at-most-once). |

### systemd example
The complete, hardened unit (with the worker's unit and an environment file) is in [`deploy/systemd/`](../deploy/systemd/); [DEPLOYMENT.md](DEPLOYMENT.md) covers Docker, Kubernetes and reverse proxies. The essentials:
```ini
[Service]
User=wsrv
RuntimeDirectory=wsrv
RuntimeDirectoryMode=0700
ExecStart=/usr/local/bin/wsrvd --config=/etc/wsrv/wsrvd.conf
Restart=on-failure
KillSignal=SIGTERM
TimeoutStopSec=15
NoNewPrivileges=yes
ProtectSystem=strict
PrivateTmp=yes
```
