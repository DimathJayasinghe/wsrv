# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: [Semantic Versioning](https://semver.org/) for the C++ API and the PHP package; the C ABI has its own `major.minor` (`wsrv_abi_version()`), additive within a major.

No release has been tagged yet. Everything below is on `main` and is what the first release (`0.1.0`, first as a `-rc` tag) will contain. Known gaps are listed in [docs/RELEASE.md](docs/RELEASE.md).

## [Unreleased]

### Added
- RFC 6455 WebSocket server: sans-I/O protocol core, handshake with Origin policy, frame codec, UTF-8 validation, fragmentation, close handshake, multi-loop epoll server, bounded queues with backpressure. Autobahn|Testsuite: 517 cases, 0 failed (plain and wss; C and PHP bindings too).
- permessage-deflate (RFC 7692) over system zlib, with a decompression-bomb guard.
- TLS (`wss://`) over OpenSSL, with certificate reload.
- Per-IP and global limits, descriptor budget check, Prometheus metrics and an admin listener.
- Pub/sub broker (topics and rooms) for C++ applications (ADR-0024).
- C ABI (`include/wsrv/wsrv.h`, ABI 0.4) and `libwsrv.so`, pull-model events, handshake authentication hook.
- `wsrvd` sidecar daemon and IPC protocol v0.
- PHP package: sidecar client and worker (pure PHP), embedded server over FFI (`Wsrv\Embedded\Server`) with the `authenticate` hook, echo and chat examples. Supports PHP 8.2 to the latest release (tested in CI).
- Composer package layout, `bin/wsrv-install-lib` (SHA-256 verified download of the native library) and a tag-triggered release workflow with checksums and build attestation (ADR-0026).
- Documentation set: requirements, architecture, ADRs, edge-case catalogue, 45 lessons.
- `wsrvd` container image (`docker/wsrvd.Dockerfile`, published to GHCR by `release.yml`, amd64 and arm64) and a Docker quickstart with a PHP worker (`examples/docker`), ADR-0026 amendment 1.
- Broker driven by connection id in the embedded server (`broker` and five `broker_max_*` config keys, ADR-0027 slice 1); the C ABI and PHP follow.

- IPC protocol 0.3: `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH` frames and the status `LIMIT`; the daemon drops a departed application's subscriptions at once (ADR-0028 slice 2).

### Fixed
- permessage-deflate: an offer that repeats a parameter is declined (RFC 7692 section 7) instead of failing the handshake; a message ended by a final block is accepted and keeps its window, with one restart per message.
