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
per_ip_connections = 100
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

## 2. Logging

- Structured, one event per line. Fields: `ts` (RFC 3339 UTC), `level`, `conn` (ID), `peer`, `event`, `code`, `msg`.
- Levels: `error` (server cannot do its job), `warn` (peer misbehaviour at scale, config oddities), `info` (lifecycle: start, stop, reload), `debug` (per-connection events), `trace` (frames; compiled out in release).
- Peer-supplied data is quoted and control-char-escaped; length-capped.
- Rate-limited logs for noisy events (bad handshakes, resets) with a suppression counter.
- No secrets, tokens, cookies, or payloads at any level above `trace`; `trace` payload logging requires an explicit unsafe flag.
- Logging is asynchronous or non-blocking relative to the loop: a slow log sink must not stall I/O (bounded queue; drop-with-counter on overflow).

## 3. Metrics (Prometheus text exposition on a separate admin listener, loopback by default)

| Metric | Type |
|---|---|
| `wsrv_connections_current` | gauge |
| `wsrv_connections_total{result="accepted|rejected_limit|handshake_failed|…"}` | counter |
| `wsrv_messages_total{direction,type}` | counter |
| `wsrv_bytes_total{direction}` | counter |
| `wsrv_close_total{code}` | counter |
| `wsrv_handshake_seconds` | histogram |
| `wsrv_loop_lag_seconds{loop}` | histogram (detects blocking handlers) |
| `wsrv_outbound_queue_bytes` | histogram |
| `wsrv_slow_consumer_drops_total` | counter |
| `wsrv_accept_errors_total{errno}` | counter |
| `wsrv_tls_handshake_failures_total` | counter |
| `wsrv_config_reload_total{result}` | counter |

Admin endpoints: `/metrics`, `/healthz` (liveness), `/readyz` (accepting & not shutting down). Admin port never exposed publicly (documented).

## 4. Lifecycle

- **Start:** validate config → check `RLIMIT_NOFILE` ≥ `max_connections + reserve` → bind → (optionally drop privileges) → start loops → log `listening`.
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
| Accept errors `EMFILE` | `wsrv_accept_errors_total`, `ulimit -n` | fd limit too low / leak |
| High `loop_lag` | handler profile | blocking user code |
| Rising outbound queue / slow-consumer drops | per-conn histogram | slow clients, broadcast too large |
| Many `handshake_failed` | log sample, Origin list | wrong origin config / scanners |
| Memory growth | pool metrics, deflate on? | context takeover memory, leak (run soak with heaptrack) |
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
- `--config=FILE` reads `key = value` lines; later arguments override earlier ones. Keys: `ipc_socket`, `ipc_token_file`, `ipc_max_frame`, `ipc_hello_timeout_ms`, `ipc_stall_ms`, the handshake-hook keys below, plus every embedded-server key (`listen_host`, `listen_port`, `threads`, `allow_origin`, `allow_missing_origin`, `max_message_size`, `max_connections`, `shutdown_grace_ms`, ...).
- The daemon prints `ipc socket ... ready` and `websocket server listening on port N` on stderr when it is ready.

### Signals and exit codes
- `SIGTERM` / `SIGINT`: graceful stop. The IPC server closes first (the socket file is removed), then the WebSocket server sends Close 1001 to every connection, waits up to `shutdown_grace_ms`, closes the rest and joins its threads. Exit code 0.
- A **second** signal during the graceful stop is not treated specially today (it does not force an immediate exit); use `SIGKILL` if a stop hangs. A stale socket file left by `SIGKILL` is recovered by the next start (it probes the socket and removes it only if nothing answers and it is ours).
- `wsrvd` installs its signal handling before it creates any other thread (signals are read from a descriptor), so every thread it starts inherits the mask.
- Exit code 1: bad configuration, unusable socket path or token file, bind failure, or an event-loop error. The reason is printed on stderr.
- `SIGHUP` is not used (no reload yet).

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
