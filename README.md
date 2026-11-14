# wsrv — WebSocket Server in C++ (working name)

A from-scratch, standards-compliant **RFC 6455** WebSocket server written in modern C++.
No Boost, no Asio, no third-party WebSocket libraries. Quality over feature count.

> Status: **1.0 release candidate for evaluation (end of M10).** RFC 6455 + permessage-deflate +
> TLS, multi-loop server, per-IP limits, pub/sub broker, Prometheus metrics, C ABI, `wsrvd` sidecar
> and a pure-PHP client are built, tested under ASan/UBSan/TSan, fuzzed, and Autobahn-clean
> (517 cases, 0 failed). Linux only. What is not done, and what stands between this and a public
> 1.0, is listed in [docs/RELEASE.md](docs/RELEASE.md). The documents below are the contract the
> code satisfies; read them before changing code.

## Why this exists

Languages such as PHP cannot hold long-lived sockets in their normal request model. wsrv is one
audited C++ core exposed through a stable C ABI and a sidecar daemon, so PHP (first) and other
languages get production-grade WebSocket servers. Open source, community-driven (license proposed: Apache-2.0).

## Goals in one paragraph

Correct (passes the full Autobahn|Testsuite), secure by default, observable, and maintainable.
The protocol logic is *sans-I/O* (pure, deterministic, fuzzable); the networking layer is a thin,
replaceable shell around it. Every design choice is traceable to an ADR in [docs/adr/](docs/adr/).

## Document map

| Document | Purpose | Read when |
|---|---|---|
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
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | Config, logging, metrics, shutdown, deployment hardening | Shipping / running |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Measured scaling, fan-out and soak numbers and their limits | Sizing or touching a hot path |
| [docs/RELEASE.md](docs/RELEASE.md) | 1.0-rc claims, evidence, known limitations, release checklist | Deciding whether to ship |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Workflow, commit rules, PR checklist, Definition of Done | Every PR |
| [SECURITY.md](SECURITY.md) | Vulnerability reporting policy | Found a vuln |
| [CLAUDE.md](CLAUDE.md) | Rules for AI coding agents working in this repo | Using an agent |

## Planned layout

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

## Quick start (Docker, recommended)

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
