# ADR-0038: A TCP transport for the IPC link

- Status: **Proposed** (2026-11-26; built at the project owner's request, the owner moves it to Accepted)
- Date: 2026-11-26
- Deciders: project owner, maintainer assistant

## Context
The IPC link between `wsrvd` and the application is a Unix domain socket (ADR-0017). That is the right default on Linux: the kernel tells the daemon who connected (`SO_PEERCRED`), the socket file has mode `0600`, and nothing reaches it from the network.

It does not work where the application and the daemon are not on the same kernel with a shared filesystem:
- **macOS and Windows developers** run `wsrvd` in Docker Desktop (or WSL2). A Unix socket cannot be shared between the host and the Docker Desktop VM in a way that works for a worker, and a Windows PHP has no `unix://` at all. Today the only choice is to put the worker in a container too (`docs/DEPLOYMENT.md` section 11).
- A worker on another machine than the daemon (a development laptop against a staging daemon over a VPN) cannot connect at all.

`docs/DEPLOYMENT.md` and the README already promise "TCP IPC" for a 1.x release (owner's decision, 2026-11-26: it can wait for 1.x, then it was asked for).

What this does **not** do: it does not make the daemon run on macOS or Windows. `wsrvd` is Linux only (epoll). A worker on the host reaches a daemon in a container, WSL2 or a VM. A daemon without Docker needs the native macOS port (kqueue, M13), which is a separate decision.

## Options considered
1. **Bind the Unix socket path inside a shared volume** (what Docker Desktop offers). Unreliable across the VM boundary on macOS and impossible for Windows PHP. Rejected: it is the problem, not a solution.
2. **A TCP listener with the same protocol** (this ADR). The IPC protocol is a framed byte stream and has never assumed more than "a reliable stream"; `HELLO` already carries a token.
3. **WebSocket or HTTP as the IPC transport.** A second framing on top of a framing, and it drags the HTTP parser into the worker path. Rejected.
4. **TLS on the IPC link now.** Right for a link across untrusted networks, wrong as a first step: it needs certificate handling on the worker side (PHP streams, Node, C++) in three clients. Deferred; the supported deployments below do not need it, and the ADR says so out loud instead of pretending.

## Decision

### 1. One more transport, the same protocol
Frames, `HELLO`, the token, the version rules, backpressure and failure behaviour (`docs/IPC_PROTOCOL.md` sections 2 to 9) are unchanged. The protocol stays `0.3`: no new frame, no new minor. Only section 1 (transport and security) gains a TCP variant. An application written for the Unix socket works over TCP by changing how it connects.

### 2. Configuration (additive, wsrvd only)
| key | meaning |
|---|---|
| `ipc_port` | TCP port for the IPC link, `0` to `65535` (`0` lets the kernel choose; `ipc_ready` logs the port, which is how the tests and `admin_port`/`publish_port` users find it). Setting it switches the TCP transport on. |
| `ipc_host` | An IP address literal to bind, default `127.0.0.1` (IPv4 or IPv6, no hostnames: no DNS lookup on the loop thread, as for every other address in the daemon). Only valid with `ipc_port`. |

**At most one** of `ipc_socket` and `ipc_port` may be set; both stops the start with a message naming both keys. With neither, the start fails as today ("ipc_socket or ipc_port is required") unless the binary was built with a default socket path. (Serving both at once is possible later; it is not needed for the use case and doubles the cases to test.) Existing configurations are unaffected.

**The official image.** Today the image sets `WSRVD_IPC_SOCKET=/run/wsrv/ipc.sock` as an `ENV` line, so `docker run -e WSRVD_IPC_PORT=9300 ...` would arrive with both keys set and fail, and the "both is an error" rule would make the TCP use case unusable in its own container. So the image no longer sets that variable: it is built with `-DWSRVD_DEFAULT_IPC_SOCKET=/run/wsrv/ipc.sock`, a default that applies **only when neither key is set**. Observable behaviour of every existing way to run the image is identical; setting `ipc_port` alone selects TCP; setting both explicitly is still an error. The release binaries have no default (an unset socket on a host is an error with a clear message, not a guessed path).

### 3. The token is mandatory over TCP, and long
With a Unix socket the kernel authenticates the peer; the token was an optional extra. A TCP peer has no kernel identity, so **the token is the only authentication** and therefore:
- `ipc_port` requires `ipc_token_file`; the start fails without it. There is no way to run an unauthenticated TCP IPC.
- The token must be at least **32 bytes** (the same floor as the publish token, ADR-0028), because it can be guessed from the network.
- The token file rules are unchanged (mode `0600`, owned by the daemon's user, never a command-line value).
- The comparison stays constant-time and the answer to every failure (wrong token, wrong major version, any other first frame) stays the same single `STATUS(request 0, INVALID_ARGUMENT)`, so the answer teaches an attacker nothing.
- The token is never logged, and neither is anything the peer sent.

### 4. The link is plaintext, and the documentation says where that is acceptable
The messages of your users and the token cross the link unencrypted. Supported:
- **Loopback** (`127.0.0.1`), the default.
- **A container's published port bound to the host's loopback** (`-p 127.0.0.1:9300:9300`): the standard Docker Desktop setup. Inside the container the daemon must bind a non-loopback address (`0.0.0.0`), because the published port arrives on the container's own interface; that is allowed, with a warning (next point) so that nobody does it by accident on a host.
- **A private network, a VPN (WireGuard), or an SSH tunnel** that you control.

Not supported: the open internet. Binding anything but a loopback address logs a `warn` line (`ipc_plaintext`) at start saying so, as the publish listener does (ADR-0028). It does not refuse: refusing would break the Docker case above.

### 5. What changes for a TCP connection (and only for one)
- **No `SO_PEERCRED` check** (there is no peer uid). The token replaces it.
- **`TCP_NODELAY`** on the accepted socket. The link carries small request/answer frames; Nagle plus delayed ACK would add tens of milliseconds to each.
- **`SO_KEEPALIVE`** with idle 30 s, interval 10 s, 3 probes. A Unix socket is closed by the kernel the moment the application's process dies; a TCP peer can vanish (a laptop closed, a cable pulled) without a FIN, and the single application slot would stay taken until the TCP stack gave up, hours later. The probes make that about 60 s, and nothing in the protocol changes.
- **A connection that has not completed `HELLO` does not hold the slot against a newcomer.** Today the single slot is taken at `accept`; on the network, anyone who can connect could keep the real application out by opening a connection and saying nothing for `ipc_hello_timeout_ms`. With this rule a newcomer simply replaces a connection that has not authenticated yet (which holds no state), so an idle or slow connection cannot lock the application out. The current connection is **looked at first** (`look_now()`, as `admit()` already does), so a `HELLO` that has already arrived is processed and not thrown away; only then, if it is still waiting for `HELLO`, it is dropped. An authenticated application is never displaced: the second authenticated one still gets `BACKPRESSURE`. The rule applies to the TCP transport only; the Unix socket behaves exactly as in 1.0 (a peer there is the same user).
- **After its last word to a peer it turns away, the daemon lingers instead of closing.** Found by the Windows CI job. The daemon answers a second application with `STATUS(BACKPRESSURE)` (and a replaced pre-`HELLO` connection, and a rejected `HELLO`, the same way) and then closes, usually before it has read that application's `HELLO`. Closing a TCP socket that holds unread data, or that receives data after the close, makes the operating system send RST instead of a clean FIN; Linux still lets the receiver read what arrived first, but **Windows discards the receive queue on RST**, so a Windows (and possibly macOS) worker would see "connection closed" instead of the status, and a plain `Client::connect` could not tell a busy daemon from a dead one. So over TCP the daemon sends the status, sends FIN (`shutdown(SHUT_WR)`), and keeps the socket, reading and discarding input, until the peer closes or `linger_timeout` (2 s) passes; then it closes. A lingering socket holds a descriptor and a small object, never the application slot, and at most 32 exist at a time: beyond that the socket is closed at once (the status was sent; only a Windows peer may lose it to a reset), logged at `debug` as `ipc_linger_full`. A Unix socket cannot be reset away from its data and is closed as before. Tests: the write-after-FIN probe (`server_still_holds_connection`) fails without lingering, the deadline test fails without the deadline, the cap test needs `ipc_linger_full`; the Windows job runs the PHP side.
- **Nothing is allocated for a connection that has not said `HELLO`.** `Peer` sizes its buffers by `max_frame`, but `ByteBuffer` reserves nothing up front (memory follows the data), so a network peer that connects and stays silent costs a descriptor and a small object, not megabytes. A test pins this (many connects, bounded memory is the reason the rule above is safe).
- **Considered and not done: a bounded set of pending connections** (each with its own HELLO deadline, the slot going to whichever authenticates first), suggested by the automated security review of slice 1. It raises the cost of displacing the real application's connection from one connect to N concurrent held connections, but it does not remove the flood case (N + 1 connections still win), and it turns the single-peer admission path into a multi-peer one, the part of the daemon that is simplest to get right today. Rate-limiting replacements per source address needs the peer address, which the daemon deliberately does not keep (ADR-0037, an address is personal data). Revisit if the port is ever meant to face anything beyond loopback or a private network; until then the port is a trust boundary (below), not a public service. A replaced connection is logged at `warn` (`app_replaced`, rate-limited by the logger, so a worker that keeps being refused leaves a trace); the failed authentications a flood causes are `app_rejected` warnings, already rate-limited.
- **Limits that remain** (stated, not hidden): someone who can reach the port can still make connections that fail authentication and force the real application to retry (a flood re-races the slot, and every failed attempt is one `app_rejected` warning, rate-limited by ADR-0037). The port is a trust boundary like a database port: reach it only from the application (loopback, a private network, a firewall rule). The token stops everything beyond that.

### 6. Health check
`wsrvd --healthcheck` does **not** probe the TCP IPC port. Probing it would, while an application is connected, be answered `BACKPRESSURE` and logged as a refused second application every 15 seconds, and it could displace a connection that is still saying `HELLO` (section 5). The daemon cannot start without its IPC listener (a bind failure ends the process), so a WebSocket port that answers already proves the listener exists. (The Unix probe is unchanged, 1.x behaviour; the image's health check goes through the admin port and never touched IPC.)

### 7. Clients
- **PHP:** `Wsrv\Client::connect('tcp://host:port', token: ...)` alongside the path form. The PHP client uses only stream functions, which exist on Windows. `Supervisor` (it forks with `pcntl`) stays Linux and macOS; on Windows the application runs its own worker script.
- **JavaScript:** nothing to do. `clients/js` is the browser and Node *WebSocket* client (ADR-0031): it talks to the daemon's WebSocket port, never to the IPC link, so the IPC transport does not touch it. (An earlier draft of this ADR listed a JavaScript IPC `connect({host, port, token})`; no such client exists in the repository. If a Node IPC worker is ever written, it takes the TCP form from the start.)
- **`TCP_NODELAY` on the client too.** `SEND_*` and `PUBLISH` are small frames going from the worker to the daemon, and Nagle's algorithm on the worker's side delays them exactly as it would on the daemon's: the PHP client sets the `tcp_nodelay` stream-context option.
- **Others:** nothing to do; the protocol is unchanged.

### 8. Observability
`ipc_ready` names the transport and, for TCP, the address actually bound (so `ipc_port=0` is usable in tests). A refused or unauthenticated connection is logged exactly as before (`app_rejected`), with no peer address unless `log_peer` is on (ADR-0037, an address is personal data).

## Slices
1. Daemon: configuration keys and their rules (and the image's built-in default socket), the TCP listener behind `IpcServer` (hygiene in section 5), `ipc_ready`, `ipc_plaintext`. Tests first (C++ test client gains a TCP connect). The container smoke test connects over TCP.
2. Client: the PHP `connect()` form (`tcp://host:port`), with tests. The unit tests use real loopback TCP and a child PHP process as the fake daemon (no `pcntl_fork`), so the macOS jobs run them as they are and **a `windows-latest` job runs the same suite**, because "it works on Windows" is the point of the feature and an untested claim otherwise (the PHP client uses only stream functions; `Supervisor` already guards `pcntl_*`, and `SIGTERM` is only named after that guard). The Linux jobs run the PHP worker against a real `wsrvd` over TCP (`php_tcp_interop`).
3. Docs: `IPC_PROTOCOL.md` section 1, `OPERATIONS.md`, `DEPLOYMENT.md` section 11 and a compose file for a host-side worker, `SECURITY_DESIGN.md` (new threat row), README, CHANGELOG, a lesson. Ships as **1.1.0** (a new minor: additive, no ABI or protocol change).

**Docker Desktop and the token file.** The token-file rules (mode `0600`, owned by the daemon's user) are not loosened to make Docker Desktop work: a file bind-mounted from a Windows drive usually appears root-owned and world-readable, and a macOS mount may not match either, so the daemon would (correctly) refuse it. The compose example for the host-side worker therefore copies the token into a named volume with the right owner and mode in a one-shot init service before the daemon starts (the pattern the Kubernetes manifest's init container already uses, and the `kind` job tests), and the guide says why.

## Consequences
- A worker on macOS, Windows or another machine can use a `wsrvd` in Docker, WSL2 or a VM. The daemon is still Linux only.
- Anyone who can reach the TCP port can attempt to authenticate; a 32-byte random token makes guessing infeasible, and the documentation puts the port behind loopback or a private network.
- No protocol change, so no client written for 1.0 breaks, and `docs/IPC_PROTOCOL.md` section 7's freeze holds.
- One more thing to keep right: the rule "a pre-`HELLO` connection holds nothing" must stay true as `Peer` grows; the test for it is named after the rule.
