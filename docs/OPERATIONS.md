# Operations

How the server is configured, observed, run, and shut down.

## 1. Configuration

Precedence (low→high): built-in secure defaults < config file (TOML) < environment (`WSRV_*`) < CLI flags. Config is parsed once into an **immutable validated snapshot**; invalid config ⇒ startup failure with all errors listed (non-zero exit). Reloadable subset (SIGHUP): TLS certs, log level, limits, origin list — applied by atomic snapshot swap.

```toml
[listen]
address = "0.0.0.0"
port = 8080
backlog = 1024
reuse_port = true

[threads]
loops = 0                 # 0 = hardware_concurrency

[limits]                  # see REQUIREMENTS §4 for defaults
max_connections = 10000
max_message_bytes = 1048576
max_handshake_bytes = 8192
per_ip_connections = 100            # 0 = no limit (ADR-0021)
per_ip_new_connections_per_s = 50   # 0 = no limit (ADR-0021)
idle_timeout_s = 60

[security]
allowed_origins = ["https://app.example.com"]
allow_missing_origin = false
trusted_proxies = []

[tls]
enabled = false
cert = "/etc/wsrv/cert.pem"
key  = "/etc/wsrv/key.pem"
min_version = "1.2"

[compression]
permessage_deflate = false

[log]
level = "info"
format = "json"           # json | text
```

Hard ceilings (compile-time) cap every limit to prevent dangerous values.

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
- **Behind a reverse proxy or NAT** every user shares one address, so the defaults can refuse real users. Raise the limits or set them to `0` and rate-limit at the proxy (real client addresses via the PROXY protocol are not implemented; EC-N21).
- **Cost:** a table of at most `2 * max_connections` entries (about 40 bytes plus the map node each), one mutex taken twice per connection life. An idle server holds no per-address state. When the table is full a new address is refused (fail closed), after one sweep of entries that carry no information.

## 2. Logging

- Structured, one event per line. Fields: `ts` (RFC 3339 UTC), `level`, `conn` (ID), `peer`, `event`, `code`, `msg`.
- Levels: `error` (server cannot do its job), `warn` (peer misbehaviour at scale, config oddities), `info` (lifecycle: start, stop, reload), `debug` (per-connection events), `trace` (frames; compiled out in release).
- Peer-supplied data is quoted and control-char-escaped; length-capped.
- Rate-limited logs for noisy events (bad handshakes, resets) with a suppression counter.
- No secrets, tokens, cookies, or payloads at any level above `trace`; `trace` payload logging requires an explicit unsafe flag.
- Logging is asynchronous or non-blocking relative to the loop: a slow log sink must not stall I/O (bounded queue; drop-with-counter on overflow).

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
| `wsrv_broker_publishes_total` | counter | done: validated publishes on the pub/sub broker (section 3.2); only once the application attaches the broker with `Broker::use_metrics(&server.shared_metrics())` |
| `wsrv_broker_deliveries_total{result}` | counter | done: `accepted` (the subscriber's loop took the message for delivery), `refused` (its task queue was full, out of memory, or the server is gone); every subscriber of a publish is one or the other |
| `wsrv_handshake_seconds` | histogram | planned (ADR-0023 section 5) |
| `wsrv_loop_lag_seconds{loop}` | histogram (detects blocking handlers) | planned |
| `wsrv_outbound_queue_bytes` | histogram | planned |
| `wsrv_config_reload_total{result}` | counter | planned (`wsrvd` reload) |

### 3.1 Admin endpoint
Off unless configured: embedded/`wsrvd` keys `admin_port` (setting it switches the listener on; 0 lets the kernel choose) and `admin_host` (default `127.0.0.1`); `wsrvd --admin-port=9100`. Plain HTTP, **no authentication**: keep it on loopback or behind a network policy (a non-loopback bind logs a warning at start). It serves exactly three pages:

| Request | Answer |
|---|---|
| `GET /metrics` | `200`, the Prometheus text of section 3 |
| `GET /healthz` | `200 ok` while loop 0 answers (liveness; a stalled loop 0 stalls this too, which is the signal) |
| `GET /readyz` | `200 ready` while the server accepts; `503 not ready` once a shutdown has begun (take the instance out of the load balancer before the grace period ends) |
| any other target | `404` (exact match: no query string, no case folding); other methods `405`, a malformed request `400` |

Bounds (ADR-0023, SECURITY_DESIGN T21): the admin listener runs on loop 0 (no extra thread); at most 8 clients at once (more are closed at once), a request head of at most 2 KiB, 5 s to send it, one request per connection, one non-blocking send for the answer (a client that cannot take the whole page in one call is dropped, so a page is never silently truncated). Prometheus example: `scrape_configs: [{job_name: wsrv, static_configs: [{targets: ["127.0.0.1:9100"]}]}]`.

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

What to watch: `wsrv_broker_deliveries_total{result="refused"}` rising means a loop's task queue is full (the publish rate outruns a loop); a `PublishReport` says the same per call. Delivery is "accepted for delivery", never acknowledged; a subscriber that cannot keep up is closed or dropped by its own connection's slow-consumer policy (`wsrv_slow_consumer_drops_total`), and the other subscribers of the same publish are unaffected. The C ABI and the PHP `Client::publish()` are not available yet (ADR-0024 section 7).

## 4. Lifecycle

- **Start:** validate config → check `RLIMIT_NOFILE` ≥ `max_connections + 64 + 4 × threads` (ADR-0022: a lower soft limit is raised to exactly that when the hard limit allows, otherwise the start fails with `FdLimit` and a log line naming needed, soft and hard) → bind → (optionally drop privileges) → start loops → log `listening`.
- **Embedded hosts (C API, PHP):** the same check runs in `wsrv_server_start` and a raise changes the soft limit of the *host* process (only upwards, only to what the configuration needs). A host that must keep a small limit lowers `max_connections`.
- **Reload (SIGHUP):** reload snapshot; on failure keep old and increment `config_reload_total{result="error"}`.
- **Graceful stop (SIGTERM/SIGINT):** stop accepting (readyz fails) → send Close 1001 to all → wait up to `shutdown_grace` (default 10 s) → force close remaining → join threads → exit 0. Second signal ⇒ immediate exit.
- **Crash policy:** invariant violation ⇒ abort with core dump (supervisor restarts); never continue in an unknown state.

## 5. Deployment hardening

- Run as non-root; bind low ports via `CAP_NET_BIND_SERVICE` or reverse proxy.
- systemd unit: `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, `PrivateDevices=yes`, `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX`, `SystemCallFilter=@system-service`, `LimitNOFILE=…`, `MemoryMax=…`, `Restart=on-failure`.
- Container: distroless/minimal image, read-only rootfs, drop all capabilities, non-root user, no shell.
- Kernel tuning (documented, not applied by the server): `net.core.somaxconn`, `net.ipv4.tcp_max_syn_backlog`, `tcp_syncookies=1`, `fs.file-max`, ephemeral port range, `tcp_keepalive_*`.
- Behind a reverse proxy: configure `trusted_proxies`, ensure proxy timeouts > heartbeat, forward `Upgrade/Connection`, pass real client IP via PROXY protocol or `X-Forwarded-For` from trusted hops only.
- Capacity planning: memory/connection × max_connections + buffer pool headroom; documented in `docs/perf/`.

## 6. Runbook (seed, expand in M10)

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

What the daemon does today (ADR-0017, IPC protocol in `docs/IPC_PROTOCOL.md`). Metrics and an admin listener arrive with M10.

### Start-up
```sh
mkdir -m 700 /run/wsrv                       # private directory, owned by the service user (or root)
install -m 600 /dev/null /etc/wsrv/ipc.token  # then put a long random token in it
wsrvd --ipc-socket=/run/wsrv/ipc.sock --ipc-token-file=/etc/wsrv/ipc.token \
      --listen-host=0.0.0.0 --listen-port=8080 --allow-origin=https://app.example.com
```
- The socket directory must not be group- or world-writable and must belong to the service user or root. The daemon creates the socket with mode `0600` and refuses to follow a symlink at that path.
- The token file must be mode `0600` and owned by the service user. There is **no** command-line or config-file value for the token (it would show in `ps` and `/proc`). Without a token only the peer-uid check protects the socket, and the daemon warns at start-up.
- `--config=FILE` reads `key = value` lines; later arguments override earlier ones. Keys: `ipc_socket`, `ipc_token_file`, `ipc_max_frame`, `ipc_hello_timeout_ms`, `ipc_stall_ms`, the handshake-hook keys below, plus every embedded-server key (`listen_host`, `listen_port`, `threads`, `tls_cert`, `tls_key`, `allow_origin`, `allow_missing_origin`, `max_message_size`, `max_connections`, `shutdown_grace_ms`, ...).
- **wss:// (TLS)**: add `--tls-cert=/etc/wsrv/cert.pem --tls-key=/etc/wsrv/key.pem` (PEM; the certificate file holds the server certificate first, then any intermediates). Setting either makes the port TLS-only (TLS 1.2 and 1.3, forward-secret AEAD suites only, ALPN `http/1.1`); plain `ws://` is not served on that port. The key file must not be readable by "others" (mode `0640` or `0600`) and must have no passphrase; an expired certificate, a key that does not match, a missing file, only one of the two options, or a build without TLS (`WSRV_ENABLE_TLS=OFF`) all stop the start-up with the reason on stderr: the daemon never falls back to plain text. Details and the threat model: ADR-0019.
- **Several certificates (SNI)**: repeat `--tls-sni=NAME=CERT_FILE:KEY_FILE` (at most 64), for example `--tls-sni=app.example.com=/etc/wsrv/app.pem:/etc/wsrv/app.key --tls-sni='*.example.org=/etc/wsrv/org.pem:/etc/wsrv/org.key'`. The client names the host it wants in its TLS hello; matching ignores case, an exact name beats a wildcard, and `*.example.org` covers exactly one label (`a.example.org`, not `example.org` or `a.b.example.org`; `*.org` is refused). A client that sends no name (or an IP address, or an unknown name) receives the `--tls-cert` certificate, so that one is required and acts as the default. Every certificate is held to the same rules as the default (valid now, key matches, key file not open to others), and all of them are re-read on `SIGHUP`; if any one fails, nothing changes. Paths must not contain `:`.
- The daemon prints `ipc socket ... ready` and `websocket server listening on port N` on stderr when it is ready.

### Signals and exit codes
- `SIGTERM` / `SIGINT`: graceful stop. The IPC server closes first (the socket file is removed), then the WebSocket server sends Close 1001 to every connection, waits up to `shutdown_grace_ms`, closes the rest and joins its threads. Exit code 0.
- A **second** signal during the graceful stop is not treated specially today (it does not force an immediate exit); use `SIGKILL` if a stop hangs. A stale socket file left by `SIGKILL` is recovered by the next start (it probes the socket and removes it only if nothing answers and it is ours).
- `wsrvd` installs its signal handling before it creates any other thread (signals are read from a descriptor), so every thread it starts inherits the mask.
- Exit code 1: bad configuration, unusable socket path or token file, bind failure, or an event-loop error. The reason is printed on stderr.
- `SIGHUP`: re-read the TLS certificate and key (`tls_cert`, `tls_key`) and use them for new connections; open connections keep the old one. Nothing else is reloaded yet. The daemon logs `tls certificate reloaded`, or `tls reload failed, keeping the old certificate: <file and reason>`, or `tls is not enabled, nothing to reload`. A failed reload never stops the server. Typical use: after the certificate renewal tool (certbot, acme.sh) writes the new files, run `kill -HUP $(pidof wsrvd)` (for systemd: `ExecReload=/bin/kill -HUP $MAINPID`).

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
