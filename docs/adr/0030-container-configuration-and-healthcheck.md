# ADR-0030: Environment configuration and a built-in healthcheck for containers

- Status: **Accepted** (2026-11-23; 1.0 production-readiness work, owner-delegated scope)
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant

## Context
`wsrvd` is configured by command-line flags and a `--config` file (ADR-0017). That fits a service manager, but containers and orchestrators configure by **environment variables** (Docker `environment:`, Kubernetes `env:`/`envFrom:` with ConfigMaps and Secrets), and they ask the container "are you healthy?" with a command (`HEALTHCHECK`, probes). The image has no shell and no `curl`, so neither a health command nor environment-based settings existed: operators had to repeat every flag in `command:` and had no way to wire a healthcheck.

## Decision

### 1. Environment variables are the base configuration layer
- Every config key has an environment variable: `WSRVD_` + the key in upper case (`WSRVD_LISTEN_PORT`, `WSRVD_MAX_CONNECTIONS_PER_IP`, `WSRVD_ALLOW_ORIGIN`). They go through exactly the same `DaemonConfig::set` as flags and file lines: one parser, one set of rules.
- **Precedence: environment first, then the arguments in order, later wins** (so `--config=FILE` and flags override the environment, and a flag after a file overrides the file, as before). One rule, no per-key exceptions.
- **Repeatable keys** (`allow_origin`, `handshake_forward_header`, `trusted_proxy`, `tls_sni`) take a **comma separated list** in one variable and add to whatever a file or flag adds; none of their values may contain a comma.
- **An empty variable counts as unset** (Compose and Helm templates produce empty strings for unset values).
- **An unknown `WSRVD_*` variable is a startup error**, naming the variable: a typo such as `WSRVD_ALLOW_ORIGINS` must not silently start a server with no Origin allow-list. Variables without the `WSRVD_` prefix are never read (this keeps Compose's own `WSRV_VERSION`/`WSRV_PORT` and everything else out of the way).
- **Secrets never come from the environment.** `WSRVD_IPC_TOKEN` is refused for the reason `ipc_token` is refused on the command line (visible in `/proc/<pid>/environ` to the same user and in `docker inspect`); tokens stay in files (`ipc_token_file`, `publish_token_file`), which Kubernetes mounts from Secrets.

### 2. `wsrvd --healthcheck`
A mode of the same binary that reads the same configuration (environment and flags), probes the **running** daemon and exits `0` (healthy) or `1` (not), printing one line of reason on failure. It never starts a server.
- If the admin listener is configured (`admin_port`): `GET /healthz` on `admin_host` (`127.0.0.1` when that is `0.0.0.0`) must answer `200`.
- Otherwise: a TCP connect to the WebSocket port must succeed (`listen_host`, or loopback for `0.0.0.0`) **and** a connect to the IPC socket must succeed.
- Each step has a 2 second deadline; the whole check is bounded and makes no allocation sized from the answer (only the first 12 bytes of the status line are read).

### 3. The image uses both
`docker/wsrvd.Dockerfile` sets the defaults as `ENV` (`WSRVD_IPC_SOCKET`, `WSRVD_LISTEN_HOST=0.0.0.0`, `WSRVD_LISTEN_PORT=8080`, `WSRVD_ADMIN_PORT=9101` on loopback) instead of `CMD` flags, and declares `HEALTHCHECK CMD ["wsrvd","--healthcheck"]`. Because the healthcheck reads the same environment, changing a setting by environment changes both the daemon and its check; one that overrides by a flag must give the healthcheck the same flag (documented). The admin port stays on the container's loopback, so it is reachable by the healthcheck and by nothing else unless the operator binds it wider.

## Consequences
+ `docker run -e WSRVD_ALLOW_ORIGIN=https://app.example.com ...` and a Kubernetes `env:` block work with no flag plumbing; `docker ps` and probes show health.
+ No new dependency: the check uses plain POSIX sockets in the composition root (`apps/wsrvd`).
− The healthcheck cannot see flags given only to the daemon; documented, and the environment is the recommended way in containers.
− Anyone who set a `WSRVD_`-prefixed variable for another purpose now gets a startup error: the prefix is ours, and failing loudly beats guessing.
