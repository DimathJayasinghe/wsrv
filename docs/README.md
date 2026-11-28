# Documentation Index

Reading order for newcomers: **PROJECT_PLAN → REQUIREMENTS → ARCHITECTURE → BINDINGS → DESIGN_PATTERNS_AND_SOLID → CODING_STANDARDS → SECURITY_DESIGN → PROTOCOL_EDGE_CASES → TESTING_STRATEGY → BUILD_AND_TOOLING → OPERATIONS**.

| Doc | One-line purpose |
|---|---|
| [PROJECT_PLAN.md](PROJECT_PLAN.md) | Vision, scope, milestones M0–M13, risks, open questions |
| [REQUIREMENTS.md](REQUIREMENTS.md) | FR/NFR with stable IDs and default limits |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Layers, components, threading, data flow, API sketch |
| [FRAGMENTATION.md](FRAGMENTATION.md) | Stream chunking vs message fragmentation, reassembly, delivery modes, outbound scheduling |
| [BINDINGS.md](BINDINGS.md) | C ABI, PHP and multi-language strategy, embedded vs sidecar, OS roadmap |
| [DESIGN_PATTERNS_AND_SOLID.md](DESIGN_PATTERNS_AND_SOLID.md) | Pattern catalogue and SOLID review rules |
| [CODING_STANDARDS.md](CODING_STANDARDS.md) | Language/style/safety rules |
| [SECURITY_DESIGN.md](SECURITY_DESIGN.md) | Threat model and controls |
| [PROTOCOL_EDGE_CASES.md](PROTOCOL_EDGE_CASES.md) | Test index of edge cases |
| [TESTING_STRATEGY.md](TESTING_STRATEGY.md) | Test pyramid, fuzzing, Autobahn, sanitizers, perf |
| [BUILD_AND_TOOLING.md](BUILD_AND_TOOLING.md) | CMake, presets, CI, dependency policy |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Docker, Compose, systemd, Kubernetes, reverse proxies, tuning, go-live checklist 
| [LARAVEL.md](LARAVEL.md) | Rooms and publishing from a Laravel app (ticket auth, Artisan worker, queued publish) |
| [LARAVEL_ECHO.md](LARAVEL_ECHO.md) | Laravel Echo and `broadcast()` on wsrv (the `wsrv` driver and `php artisan wsrv:echo`; wsrv 1.1, ADR-0042) |
| [UPGRADING.md](UPGRADING.md) | What to do when moving from one release to the next (1.0 to 1.1: the publish token rule) |
| [OPERATIONS.md](OPERATIONS.md) | Config, logging, metrics, lifecycle, hardening |
| [BENCHMARKS.md](BENCHMARKS.md) | wsrv against Workerman, Swoole and Reverb: numbers, noise, limits, how to rerun |
| [adr/](adr/) | Architecture decision records |
| [learning/](learning/) | Lessons written as we build: what, why, how |

Planned later (created by their milestone, not before): `IPC_PROTOCOL.md` (M11), `RELEASE_CHECKLIST.md` (M10), `perf/` (M7+), user guides and per-language quick starts (M12+).

## Documentation rules
- Docs change in the same PR as behaviour.
- Requirement/edge-case IDs are stable; never renumber, only deprecate.
- Every public API symbol has reference docs (Doxygen); every binding has a 5-minute quick start.
