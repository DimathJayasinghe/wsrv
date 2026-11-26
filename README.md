# wsrv: a WebSocket server for PHP, with a C++ core

Hold thousands of WebSocket connections in one small daemon and write the logic in PHP (or any
language that can call a C ABI). A from-scratch **RFC 6455** implementation in modern C++23: no Boost,
no Asio, no third-party WebSocket library. Open source under the [Apache License 2.0](LICENSE).

**New here?** [Get started in five minutes](docs/GETTING_STARTED.md) (a browser page, a PHP worker and a
reconnect you can watch), see [what has been checked and how to check it yourself](docs/EVIDENCE.md).

> Status: **1.0.0**, stable. The 1.0 line freezes the IPC protocol, the C ABI and the PHP API (see
> [Stability](#stability)); what was checked before release, and what is known and accepted, is in
> [docs/RELEASE.md](docs/RELEASE.md), and changes are in [CHANGELOG.md](CHANGELOG.md). The server is Linux only
> (x86-64 and aarch64); on macOS and Windows develop with Docker or WSL2 ([docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) section 11). A native macOS server is planned for a future 1.x release; since 1.1 a worker on macOS or Windows can reach a daemon in Docker over [TCP IPC](examples/host-worker).

## What you get

- **A daemon, `wsrvd`, that owns the sockets.** RFC 6455 and permessage-deflate (RFC 7692), TLS, Origin
  checks on by default, per-IP and global limits, and a bound on every queue and buffer.
- **Rooms and push.** A pub/sub broker and an HTTP publish endpoint, so a controller or a queue job can
  send to browsers without holding a connection.
- **A PHP package** (`composer require wsrv/client`, PHP 8.2 or newer, no extension for the sidecar): a
  worker that reconnects after a daemon restart, a `Publisher`, and a [Laravel guide](docs/LARAVEL.md).
- **A JavaScript client** that reconnects with backoff and rejoins its rooms
  ([clients/js](clients/js/README.md)).
- **Operations built in:** Prometheus metrics, `/healthz` and `/readyz`, graceful shutdown, and a
  [deployment guide](docs/DEPLOYMENT.md) (Docker, Compose, systemd, Kubernetes, reverse proxies).
- **Tested hard:** Autobahn|Testsuite (517 cases, 0 failed, also over `wss://`), the whole suite under
  ASan, UBSan and TSan, and 14 fuzz targets. [What was checked, and how to check it yourself](docs/EVIDENCE.md).

```
browser ──WebSocket──▶ wsrvd (C++: sockets, TLS, limits, rooms) ──Unix socket──▶ your PHP worker
                           ▲
controller / queue job ──HTTP POST /publish (token)
```

PHP never runs on the daemon's I/O threads: a slow handler does not block the sockets (pings, handshakes and
limits keep working), and a worker that stalls for good is cut off after a timeout and its clients are closed with 1001.

## Run it in 60 seconds (PHP, Docker; no C++ and no compiler)

```sh
git clone https://github.com/DimathJayasinghe/wsrv && cd wsrv/examples/docker
docker compose up -d                       # port 8080 or 8000 busy? WSRV_PORT=18080 WSRV_WEB_PORT=18000 docker compose up -d
```

Open <http://localhost:8000>, press **Connect** and **Send**: the PHP worker echoes your message. Press
**Join** to enter `room.1`. Prefer a terminal? Any WebSocket client works:

```sh
npx wscat -c ws://localhost:8080/
> hello                                    # < hello            (the PHP worker echoes)
> join room.1                              # < joined room.1
```

In a second terminal, publish from a separate process, as a controller or a queue job would:

```sh
docker compose run --rm worker php /app/publish.php room.1 "hello from outside"
# subscribers=1 accepted=1 refused=0   and the first terminal prints: hello from outside
```

Then `docker compose restart wsrvd` and watch the page reconnect and join its room again by itself.
Step by step, for every stack: [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md).

Use it in your own project:

| You want | Do this |
|---|---|
| The daemon | `docker run ghcr.io/dimathjayasinghe/wsrvd:1.0.0` (amd64 and arm64), or the checksummed binary from the [releases](https://github.com/DimathJayasinghe/wsrv/releases) |
| The PHP client | `composer require wsrv/client` (PHP 8.2 or newer, no extension needed for the sidecar) |
| Rooms and push from a controller or job | `Wsrv\Publisher`, see [bindings/php/README.md](bindings/php/README.md) |
| Laravel | [docs/LARAVEL.md](docs/LARAVEL.md) (ticket login, an Artisan worker, queued publishing; tried against Laravel 13) |
| Production notes | TLS, limits, metrics, hardening: [docs/OPERATIONS.md](docs/OPERATIONS.md) |

Browser (`Origin`) checks are on by default; the example allows command-line clients with `--allow-missing-origin=true`: remove that for anything browser-facing.

## How it compares

Measured against Workerman 5, Swoole and Laravel Reverb on the same machine (512 connections for echo,
5 000 for the broadcast; [docs/BENCHMARKS.md](docs/BENCHMARKS.md) has every number, the noise, and the
harness in [`bench/compare/`](bench/compare/README.md) to run yourself):

| | wsrv | Workerman 5 | Swoole | Reverb |
|---|---|---|---|---|
| Echo, four CPUs (messages/s) | **192,036** | 100,582 | 112,610 | n/a |
| Echo, one CPU (messages/s) | 99,287 | 112,135 | **126,032** | n/a |
| Broadcast to 5 000 subscribers, four CPUs (ms to the last one) | **31.1** | 58.6 | 52.2 | 89.5 |
| Memory per idle connection, 10 000 connections | **8.3 KiB** | 8.8 KiB | 8.9 KiB | 20.5 KiB |

wsrv is ahead when it has several cores (about 1.7 to 1.9 times the echo of the others with 512 connections)
and behind per core on echo, by 11 to 21 percent on one CPU: the daemon and the PHP worker are two
processes, the others are one. One laptop, a load generator written by this project, small messages
and no TLS: read the page before you quote a number.

## Stability

From 1.0, across every 1.x release:

- the **IPC protocol** between `wsrvd` and a worker (version 0.3): frames and fields are never removed
  or changed, and minors only add;
- the **C ABI** (`include/wsrv/wsrv.h`), which CI checks against the exported symbols;
- the **PHP package** follows [semantic versioning](https://semver.org/), and the daemon and the package
  are versioned together, so you can upgrade them one at a time (daemon first);
- the **names of the log fields and events** ([docs/OPERATIONS.md](docs/OPERATIONS.md) section 2): new ones
  may appear, none is renamed or removed; the message text is not promised.

Not promised: the JavaScript client in `clients/js` (you copy its one file into your project; it is not on
npm), the C++ headers under `src/`, metrics marked experimental in
[docs/OPERATIONS.md](docs/OPERATIONS.md), and anything under `bench/` or `tests/`. How to upgrade a
running system with no more than a reconnect: [docs/DEPLOYMENT.md, section 10](docs/DEPLOYMENT.md#10-upgrading).
Security reports: [SECURITY.md](SECURITY.md).

## Documentation

| You want to | Read |
|---|---|
| Get started, for your stack (Docker, PHP, Laravel, macOS and Windows, the browser client) | [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) |
| See what was checked, and check it yourself | [docs/EVIDENCE.md](docs/EVIDENCE.md) |
| Put it in production (Docker, systemd, Kubernetes, proxies, sizing, go-live checklist) | [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) |
| Configure, monitor and harden it (limits, TLS, metrics, shutdown) | [docs/OPERATIONS.md](docs/OPERATIONS.md) |
| Use it from PHP | [bindings/php/README.md](bindings/php/README.md) |
| Use it from Laravel | [docs/LARAVEL.md](docs/LARAVEL.md) |
| Write a client or another language binding | [docs/IPC_PROTOCOL.md](docs/IPC_PROTOCOL.md), [docs/BINDINGS.md](docs/BINDINGS.md) |
| Know what it costs and how it compares | [docs/BENCHMARKS.md](docs/BENCHMARKS.md), [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| Judge how safe it is | [docs/SECURITY_DESIGN.md](docs/SECURITY_DESIGN.md), [docs/RELEASE.md](docs/RELEASE.md) |

## For contributors

Languages such as PHP cannot hold long-lived sockets in their normal request model. wsrv is one
audited C++ core exposed through a stable C ABI and a sidecar daemon, so PHP (first) and other languages
get a production-grade WebSocket server. Contributions are welcome under the
[Code of Conduct](CODE_OF_CONDUCT.md); start with [CONTRIBUTING.md](CONTRIBUTING.md).

The goals: correct (passes the full Autobahn|Testsuite), secure by default, observable, and
maintainable. The protocol logic is *sans-I/O* (pure, deterministic, fuzzable); the networking layer is
a thin, replaceable shell around it. Every design choice is traceable to an ADR in [docs/adr/](docs/adr/).
The documents below are the contract the code satisfies; read them before changing code.

## Document map

| Document | Purpose | Read when |
|---|---|---|
| [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) | Five-minute quickstart and one path per stack | First time here |
| [docs/EVIDENCE.md](docs/EVIDENCE.md) | Claims, the command that checks each, last results and limits | Deciding whether to trust it |
| [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md) | Vision, scope, milestones, exit criteria, risks | Starting any work |
| [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) | Functional & non-functional requirements, default limits | Designing/accepting a feature |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, threading, data flow, public API | Adding/changing a component |
| [docs/FRAGMENTATION.md](docs/FRAGMENTATION.md) | TCP chunking vs WebSocket fragmentation, reassembly, streaming, outbound scheduling | Touching framing/messages |
| [docs/BINDINGS.md](docs/BINDINGS.md) | C ABI, PHP + multi-language strategy, embedded vs sidecar, OS roadmap | Integrating from another language |
| [GOVERNANCE.md](GOVERNANCE.md) | Roles, decisions, binding tiers (draft) | Joining the community |
| [docs/DESIGN_PATTERNS_AND_SOLID.md](docs/DESIGN_PATTERNS_AND_SOLID.md) | Which pattern solves which problem; SOLID rules and review checks | Designing a class |
| [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) | Language rules, naming, errors, memory, concurrency | Every PR |
| [docs/SECURITY_DESIGN.md](docs/SECURITY_DESIGN.md) | Threat model, controls, secure-coding rules | Every PR touching input/IO |
| [docs/PROTOCOL_EDGE_CASES.md](docs/PROTOCOL_EDGE_CASES.md) | Exhaustive edge-case checklist mapped to tests | Touching protocol/net code |
| [docs/TESTING_STRATEGY.md](docs/TESTING_STRATEGY.md) | Test pyramid, fuzzing, Autobahn, sanitizers, perf | Writing tests |
| [docs/BUILD_AND_TOOLING.md](docs/BUILD_AND_TOOLING.md) | CMake, presets, CI, linters, dependency policy | Setting up / changing build |
| [docs/LARAVEL.md](docs/LARAVEL.md) | Rooms and publishing from a Laravel app | Using wsrv from Laravel |
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | Config, logging, metrics, shutdown, deployment hardening | Shipping / running |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Measured scaling, fan-out and soak numbers and their limits | Sizing or touching a hot path |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | wsrv against Workerman, Swoole and Reverb: numbers, noise, limits, how to rerun | Choosing a server, or quoting a number |
| [docs/RELEASE.md](docs/RELEASE.md) | 1.0-rc claims, evidence, known limitations, release checklist | Deciding whether to ship |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Workflow, commit rules, PR checklist, Definition of Done | Every PR |
| [SECURITY.md](SECURITY.md) | Vulnerability reporting policy | Found a vuln |
| [CHANGELOG.md](CHANGELOG.md) | What changed, per release | Upgrading |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | How we treat each other | Joining the community |
| [CLAUDE.md](CLAUDE.md) | Rules for AI coding agents working in this repo | Using an agent |

## Source layout

```
include/wsrv/      public API headers only (stable surface)
src/base/          Result, buffers, clock, logger interface
src/crypto/        SHA-1, Base64, CSPRNG wrapper
src/http/          minimal HTTP/1.1 upgrade request parser / response writer
src/protocol/      sans-I/O: handshake, frame codec, UTF-8, message assembly, connection state machine
src/extensions/    permessage-deflate (RFC 7692)
src/net/           sockets, epoll poller, event loop, timers, acceptor, transports (plain/TLS)
src/server/        connection, registry, handler API, pub/sub, server facade + builder
tests/             unit/ integration/ fuzz/ autobahn/ bench/
examples/          echo, chat
docs/              this documentation set
```

## Build from source (contributors)

```bash
make image      # once: builds the dev image (GCC 14, Clang 18, CMake, sanitizers, ...)
make shell      # shell inside the container; same path as on the host, passwordless sudo
# inside the container:
cmake --preset dev && cmake --build --preset dev && ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check check-banned check-includes header-check
```

Other presets: `dev-clang`, `release`, `tsan`, `tidy`, `coverage` (see [Lesson 01](docs/learning/01-build-system-and-quality-gates.md)).
If a build ignores an edit you can see on the host, run `make restart` (Docker Desktop bind-mount quirk, see docs/BUILD_AND_TOOLING.md §10).

## Try it

```bash
# inside the container, after the build above
build/dev/examples/echo --port 9001 --allow-missing-origin           # echo server (Autobahn target, browser demo backend)
build/dev/examples/chat --port 9002 --admin-port 9100 --allow-missing-origin   # rooms: join / say / members / rooms / leave
curl http://127.0.0.1:9100/metrics                                     # Prometheus text, includes wsrv_broker_*
```

Browser demo: [examples/demo/README.md](examples/demo/README.md). Chat protocol: header of `examples/chat/chat.hpp`.

## Toolchain baseline

C++23, GCC ≥ 13 or Clang ≥ 17, CMake ≥ 3.25, Linux first (epoll). Details: [BUILD_AND_TOOLING.md](docs/BUILD_AND_TOOLING.md).
