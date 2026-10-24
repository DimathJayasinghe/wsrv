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
