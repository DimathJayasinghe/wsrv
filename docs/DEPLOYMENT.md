# Deploying wsrv

How to run `wsrvd` and your PHP worker in production: Docker, Compose, systemd, Kubernetes, behind a reverse proxy, with the limits tuned. Every setting here is a `WSRVD_<KEY>` environment variable (ADR-0030) or the same key as a `--flag`; docs/OPERATIONS.md explains what each does.

| You run | Use |
|---|---|
| One Docker host | [`examples/docker/compose.yaml`](../examples/docker/compose.yaml) (the 60-second quickstart is the same file) |
| A Linux VM | [`deploy/systemd/`](../deploy/systemd/) |
| Kubernetes | [`deploy/kubernetes/wsrv.yaml`](../deploy/kubernetes/wsrv.yaml) (applied to a kind cluster by CI on every change) |

## 1. The shape of a deployment

```
browser ──wss──► reverse proxy ──ws──► wsrvd ──Unix socket (0600)──► your worker (PHP)
                  (TLS, routing)       (sockets, rooms)               (your code)
                                          ▲
        any process ──HTTP POST /publish──┘   (queued jobs, cron, other services)
```

- **One daemon serves one application connection.** One worker process per `wsrvd`. The daemon is a native, multi-threaded server and is not the bottleneck; your PHP is. To use more cores or more machines run several **pairs** (a `wsrvd` and its worker) and balance the browsers across them (sections 6 and 7).
- **The worker must run as the same user as `wsrvd`** (the socket is mode 0600 and the daemon checks the peer's uid, ADR-0017). In containers that is uid 10001 for both.
- **Never expose the IPC socket, the publish port or the admin port to the internet.** The browsers reach one port, the WebSocket port, through your proxy.

## 2. Configuration

Everything is a key. Put it where your platform keeps configuration:

| Where | How |
|---|---|
| Docker / Compose | `environment: {WSRVD_ALLOW_ORIGIN: "https://app.example.com"}` |
| systemd | `EnvironmentFile=/etc/wsrv/wsrvd.env`, lines `WSRVD_KEY=value` |
| Kubernetes | a ConfigMap and `envFrom`; set `enableServiceLinks: false` in the pod spec (a Service called `wsrvd` would otherwise add `WSRVD_SERVICE_HOST`, `WSRVD_PORT=tcp://...`; the daemon ignores those exact names, ADR-0030 A1, but you need not carry them) |
| A config file | `--config=/etc/wsrv/wsrvd.conf`, lines `key = value` |
| Flags | `--max-connections=20000` |

Precedence: **environment, then `--config` and flags in order; later wins.** A repeatable key (`allow_origin`, `trusted_proxy`, `handshake_forward_header`, `tls_sni`) takes a comma list in one variable. An empty variable counts as unset; an **unknown `WSRVD_*` variable stops the start** and names itself, so a typo cannot silently weaken a setting. **Secrets are never environment variables**: put each token in a file (mode 0600) and name the file (`ipc_token_file`, `publish_token_file`); Kubernetes mounts these from a Secret.

The settings most deployments need:

| Setting | Why |
|---|---|
| `allow_origin` | The sites whose pages may open a socket. Required unless you really want any origin (`allow_missing_origin` is for non-browser clients only). |
| `trusted_proxy` | Your reverse proxy's address or CIDR, so limits and logs see real clients (OPERATIONS 1.2.1). |
| `max_connections`, `max_connections_per_ip`, `max_new_connections_per_ip_per_second` | Capacity and abuse limits (OPERATIONS 1). |
| `admin_port`, `admin_token_file` | `/healthz`, `/readyz` and `/metrics` (loopback, no authentication by default); `admin_token_file` makes `/metrics` need a bearer token, for an admin port a scraper reaches over a network (OPERATIONS 3.1). |
| `broker`, `publish_port`, `publish_token_file` | Rooms and HTTP publishing from other processes. |
| `tls_cert`, `tls_key` | TLS on `wsrvd` itself; or terminate TLS at the proxy and leave these out. |
| `log_level`, `log_peer` | One structured line per event on stderr (`docker logs`, `kubectl logs`, `journalctl -u wsrvd`), at `info` by default; the client address is left out unless `log_peer` is on (OPERATIONS 2). |

## 3. Health

- **`wsrvd --healthcheck`** exits 0 when the running daemon is fine: with an admin port it asks `GET /healthz`, otherwise it connects to the WebSocket port and the IPC socket. It reads the same environment as the daemon. The container image declares it as its `HEALTHCHECK`, so `docker ps` shows `healthy`.
- **`/readyz`** (admin port) is 200 while serving and 503 while shutting down: use it as the readiness probe so a terminating instance leaves the balancer first.
- **`/metrics`** is Prometheus text (OPERATIONS 3).

## 4. Starting and stopping

- **SIGTERM** is graceful: the daemon stops accepting, sends Close 1001 to every client, waits up to `shutdown_grace_ms` (10 s default), then exits 0. systemd uses `KillSignal=SIGTERM` and `TimeoutStopSec=20`; Kubernetes sends it and waits `terminationGracePeriodSeconds`.
- **Clients must reconnect.** A restart, a deploy or a node drain closes every socket with 1001. Browsers do not retry on their own: use the reconnecting client in `clients/js` (`WsrvClient`: exponential backoff with jitter, a fresh ticket for every attempt, rooms re-joined after every reconnect; ADR-0031) or write the same loop yourself. Without it, the first deploy leaves every open page dead.
- **The worker follows the daemon.** The IPC link dies with the daemon and `Worker::run` throws `ConnectionClosed`. Run the worker under `Wsrv\Supervisor` (`Supervisor::forSocket($socket, $token)->run(fn (Worker $w) => ...)`): it reconnects with backoff, tells apart "another application is still connected" (retried) from a wrong token (fatal), reports a throwing handler without dropping the link (a send larger than the daemon's frame, `MessageTooLarge`, is such an error: it is refused before anything is written, so the link is fine), and stops on `SIGTERM`. Keep your process supervisor too (systemd `Restart=`, Compose `restart:`, a Kubernetes restart policy) for the crash the Supervisor cannot survive.
- **The worker leaving closes the browsers.** When the application disconnects (a deploy of the worker, a crash), the daemon closes the browser connections it knew about with **1012** (service restart) after `shutdown_grace_ms`, so a worker restart looks to the client like a daemon restart: it reconnects, re-joins its rooms and the new worker answers. Those sockets are closed after the grace period even if a new worker has already connected (it never sees their old traffic: IPC_PROTOCOL.md section 6), so plan for clients to reconnect on every worker deploy.
- **`SIGHUP`** re-reads the TLS certificate and key without dropping anyone (OPERATIONS section 7).
- A **rolling update** (Kubernetes `maxUnavailable: 0`, or one VM at a time) turns over the pairs one after another; each pair's clients reconnect, to the others while it restarts.
- **Give the ingress a few seconds to forget a pod.** The daemon stops accepting at SIGTERM, while an ingress controller or load balancer may route new upgrades to a terminating pod for a few seconds more. The reference manifest runs `sleep 5` in a `preStop` hook (before SIGTERM; `terminationGracePeriodSeconds` 30 covers it plus the 10 s grace). Behind a cloud load balancer, also lower its deregistration delay to a few seconds, or clients that connect during a deploy are refused.
- **The token file must be yours.** `wsrvd` refuses a `publish_token_file` or `ipc_token_file` that group or others can read, or that another user owns (root excepted), and a Kubernetes Secret volume or a Docker/Swarm `secrets:` mount is root-owned and world- or group-readable, so it is refused as mounted. Copy it first: an init container (the reference manifest) or `install -m 0600 -o <uid> /run/secrets/token /run/wsrv/token` in an entrypoint (`examples/docker/compose.yaml` does the same in its `token-init` service), and point the key at the copy. From 1.1 the **publish token must be one `wsrvd --generate-token=FILE` made** (ADR-0039; the IPC token only over TCP): make it once with the image (`docker run --rm -u "$(id -u)" -v "$PWD:/out" ghcr.io/dimathjayasinghe/wsrvd:1.1.0 --generate-token=/out/publish.token`) and create the Secret from the file (`kubectl create secret generic wsrv-secret --from-file=publish-token=publish.token`), or let each pod make its own when only the worker in the same pod publishes. Upgrading from 1.0 means rotating a token you made yourself: the daemon refuses it and says why.

## 5. Behind a reverse proxy

The proxy must pass the upgrade, keep the connection open longer than the heartbeat (the daemon pings after 30 s of silence, `session.ping_interval`), and say who the client is.

**nginx**

```nginx
location /ws {
    proxy_pass http://127.0.0.1:8080;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_read_timeout 120s;      # longer than the 30 s ping: the default 60 s also works, a short one does not
    proxy_send_timeout 120s;
    proxy_buffering off;
}
```

**Caddy** (WebSocket upgrade and `X-Forwarded-For` are automatic)

```caddyfile
app.example.com {
    reverse_proxy /ws 127.0.0.1:8080
}
```

**Traefik, Envoy, HAProxy, AWS ALB and other proxies:** all upgrade WebSockets; keep their idle timeout above 30 s (ALB's default of 60 s is fine) and make sure they append the client to `X-Forwarded-For`. Then set `WSRVD_TRUSTED_PROXY` to their address range. A PROXY-protocol-only TCP balancer is not supported yet: raise the per-IP limits instead (ADR-0029 section 6).

Match `allow_origin` to the page's origin as the browser sends it (`https://app.example.com`), not to the proxy.

## 6. Using more than one core

A daemon's WebSocket side already spreads connections over `threads` event loops; your worker is the single PHP process. If it is the bottleneck, run several pairs on one machine, each with its own socket directory and port:

```
wsrvd#1  :8081  /run/wsrv1/ipc.sock  ←→ worker#1
wsrvd#2  :8082  /run/wsrv2/ipc.sock  ←→ worker#2
nginx:  upstream wsrv { server 127.0.0.1:8081; server 127.0.0.1:8082; }   # least_conn
```

Rooms are per daemon (see below): a message for a room must be published to every daemon.

## 7. Using more than one node

Rooms and subscriptions live inside each `wsrvd`. To reach every subscriber, **publish to every daemon**: the PHP `ClusterPublisher` takes several endpoints and POSTs to each of them (`new ClusterPublisher(['http://a:9200', 'http://b:9200'], $token)`), one after another, with one shared publish token. A node that fails does not stop the others; `$result->failures` names it, and if no node answers it throws `PublishFailed`. Keep its timeout short: per-node timeouts add up. There is no Redis (or other) backplane yet: it is a planned 1.x feature, not part of 1.0. Browsers need no stickiness (a WebSocket stays on the node it connected to; a reconnect may land elsewhere and re-subscribes).

## 8. Sizing and operating-system limits

- **Descriptors:** one per connection plus a few. `wsrvd` checks `RLIMIT_NOFILE` at start and refuses a `max_connections` it cannot honour (ADR-0022). systemd: `LimitNOFILE=1048576` (set in the unit). Docker: `--ulimit nofile=1048576:1048576`. Kubernetes: check `ulimit -n` inside the pod (it comes from the container runtime) and raise it there if it is below `max_connections`.
- **Memory:** measured on the benchmark rig ([BENCHMARKS.md](BENCHMARKS.md): `wsrvd` and one PHP worker, idle connections, no compression, as the container's memory counter sees it, kernel socket memory included): about **8 KiB per connection** at 10 000 connections (98 MB peak over a 14 MB baseline). Connections that send and receive hold more while messages are in flight, so plan **16 KiB per connection** as a starting budget and measure your own traffic. An idle connection costs a few KiB of buffers plus its kernel socket memory; permessage-deflate adds a compressor and an inflater per connection once used (OPERATIONS 1.1 gives zlib's formula). Do not guess: start a test instance, open your expected number of connections and read `docker stats` / RSS, then set `max_connections` and the container limit from that.
- **Kernel:** `net.core.somaxconn` (accept queue, `4096`), `net.ipv4.tcp_max_syn_backlog`, `net.ipv4.ip_local_port_range` on the **proxy** (it opens one port per client), `fs.file-max`. These are the operator's to set; `wsrvd` never changes them.
- **Threads:** `threads` event loops; start at the number of cores you give it (not more).

## 9. Checklist before you go live

- [ ] `allow_origin` lists only your sites; `allow_missing_origin` is off.
- [ ] TLS: at the proxy or on `wsrvd` (`wss://` only on the public side).
- [ ] `trusted_proxy` is your proxy; per-IP limits are on (not `0`).
- [ ] The IPC socket directory is mode 0700 and the worker runs as the daemon's user.
- [ ] The publish port and the admin port are not reachable from outside.
- [ ] Your log collection reads the container's or unit's stderr, and an alert exists for `level=error` and for a burst of `event=app_rejected` (OPERATIONS 2.3). The default does not log client addresses; decide whether you want `log_peer`.
- [ ] The healthcheck/readiness probe is wired; `SIGTERM` reaches the daemon (not a shell wrapper).
- [ ] Clients reconnect with backoff (`clients/js`, or your own loop); the worker runs under `Supervisor` and your process supervisor restarts it.
- [ ] `max_connections` is below the descriptor limit and the memory you have.
- [ ] You know how you will upgrade: daemon first, then workers (section 10).

## 10. Upgrading

The two halves of a deployment (the `wsrvd` daemon and the PHP package that talks to it) are versioned together (`1.0.x` daemon with `1.0.x` package), and the wire between them has its own rule, so you can upgrade them one at a time.

**The IPC protocol** (docs/IPC_PROTOCOL.md section 7) is `0.3`. Its guarantee for the whole `1.x` line: frames and fields are never removed or changed, minors are **additive only**, and a major bump (which would make an old client refuse to start) happens only with wsrv 2.0. In practice:

| Combination | Works? |
|---|---|
| a newer daemon with an older worker package of the same major | yes, an older minor in `HELLO` is accepted; only the features of that minor are available. Tested: a `0.1.0-rc.1` worker against a `1.0` daemon |
| a newer worker package with an older daemon | basics yes; a feature the daemon does not know (for example rooms against a daemon below minor 3) is refused with a status `INVALID_ARGUMENT` or closes the link, never silently wrong |
| a daemon started with `handshake_hook` and a worker below minor 2 | refused at `HELLO` (the worker could not answer upgrades) |

So **upgrade the daemon first, then the workers**.

**Restarting one node (what clients see):** a daemon and its worker are one pair, and a browser's socket lives on the daemon.
1. *Worker upgrade* (stop the old worker, start the new one): the daemon has exactly one application connection, so there is a gap. In it the daemon keeps accepting WebSocket upgrades and queues their events for the next application (with `handshake_hook` on it answers `503` instead). The connections the old worker knew are closed with 1012 after `shutdown_grace_ms` (IPC_PROTOCOL section 6), so every connected client reconnects once per worker deploy: use the reconnecting client (`clients/js`).
2. *Daemon upgrade*: every socket gets 1001 and is closed after the grace period; clients reconnect to the new daemon, and the supervised worker (`Supervisor`) reconnects to it by itself.
3. *Several nodes behind a load balancer*: drain and upgrade one node at a time; its clients reconnect to the others.

**What 1.x does not promise:** the C++ headers under `src/` (only `include/wsrv/wsrv.h` is the stable C surface), the metric names marked experimental in OPERATIONS section 3, and anything in `bench/` or `tests/`.

## 11. macOS and Windows

The server (`wsrvd`, `libwsrv`) runs on **Linux only**: it is built on epoll. Your users' devices do not matter, because browsers on any system connect to it; only the machine that runs the server does. Production is therefore Linux, and on a Mac or a Windows PC you develop against a Linux `wsrvd`:

- **Windows:** WSL2 (a real Linux kernel: install and run everything as on Linux) or Docker Desktop.
- **macOS:** Docker Desktop. The image is multi-arch, so it runs natively on Apple Silicon.
- **A worker on the host, the daemon in Docker (wsrv 1.1).** The IPC link can be TCP (`ipc_port`, ADR-0038), so your PHP worker runs natively on macOS or Windows (PHP 8.2 or newer, `composer require wsrv/client`, `Client::connect('tcp://127.0.0.1:9300', $token)`) and reaches `wsrvd` in Docker Desktop through a port published on the host's loopback. [`examples/host-worker`](../examples/host-worker) is the whole setup. Three things to know: the link is **not encrypted** (publish it on `127.0.0.1` only, or use a VPN or an SSH tunnel); the **token is mandatory** and must be one `wsrvd --generate-token=FILE` made (256 random bits in a fixed form; the daemon refuses any other over TCP, however long, because a chosen token can be guessed), because over TCP it is the only authentication; and the token file must still be mode `0600` and owned by the daemon's user, so it lives in a volume (the compose file creates it there) and not as a file mounted from your drive.
- **Or run the worker next to the daemon.** With a Unix socket (the default, and the one to use in production on Linux) the worker must be on the same kernel: put it in a container beside `wsrvd`, sharing a volume for the socket (as `examples/docker` does), or run both inside WSL2. Only the WebSocket port (and the publish port, if you use it) is published to the host.
- **What runs natively on a Mac or a Windows PC:** the PHP package's sidecar client and worker (their tests run on Apple Silicon macOS and on Windows in CI on every change; `Supervisor`'s signal handling needs `pcntl`, so on Windows stop the worker with Ctrl+C or your service manager), and on a Mac the JavaScript client too. `wsrv-install-lib` refuses on macOS and Windows instead of writing a Linux file, and says so. The embedded server (`Wsrv\Embedded\Server`) needs `libwsrv`, so it is Linux only too.
- **Not tested:** Docker Desktop on a real Mac or a real Windows PC (hosted macOS and Windows runners have no Docker for Linux containers; the same arm64 image is started on native arm64 Linux runners by the release job, and nobody on the project has a Mac), Intel Macs, and the JavaScript client on native Windows. What CI does check on Windows is the PHP client against a daemon stand-in over real loopback TCP; the end-to-end path (a host worker, a published port, a container) is checked on Linux by the container smoke test and `php_tcp_interop`.

**Planned for a future 1.x release:**
- **A native macOS server** (a kqueue poller), then Windows. A build probe in CI (Apple clang 17, 2026-11-26, TLS and deflate off) shows how far away this is: `base`, `protocol`, `http`, `crypto` and `ipc` already compile; what does not is `net`, `server`, `capi` and `wsrvd`, almost entirely on `<sys/epoll.h>`, plus one `getrandom` call.

These have no dates. The 1.x promise (IPC protocol, C ABI, PHP API) does not change when they arrive.

