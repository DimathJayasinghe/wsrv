# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: [Semantic Versioning](https://semver.org/) for the C++ API and the PHP package; the C ABI has its own `major.minor` (`wsrv_abi_version()`), additive within a major.

Known gaps are listed in [docs/RELEASE.md](docs/RELEASE.md). `0.1.0-rc.1` and `0.1.0-rc.2` were pre-releases of what is below.

## [Unreleased]

## [0.1.0] - 2026-11-23

The first release meant for developers to build with. Not 1.0: APIs can still change in a minor version.

### Added
- The real client address behind a reverse proxy: `trusted_proxy` (addresses/CIDRs) and `client_ip_header` keys; the per-IP limits and the peer address in events then follow the client, not the proxy. Header ignored from untrusted peers, rightmost untrusted `X-Forwarded-For` entry, anything unusable falls back to the proxy address (ADR-0029). PROXY protocol is not implemented yet.
- RFC 6455 WebSocket server: sans-I/O protocol core, handshake with Origin policy, frame codec, UTF-8 validation, fragmentation, close handshake, multi-loop epoll server, bounded queues with backpressure. Autobahn|Testsuite: 517 cases, 0 failed (plain and wss; C and PHP bindings too).
- permessage-deflate (RFC 7692) over system zlib, with a decompression-bomb guard.
- TLS (`wss://`) over OpenSSL, with certificate reload.
- Per-IP and global limits, descriptor budget check, Prometheus metrics and an admin listener.
- Pub/sub broker (topics and rooms) for C++ applications (ADR-0024).
- C ABI (`include/wsrv/wsrv.h`, ABI 0.4) and `libwsrv.so`, pull-model events, handshake authentication hook.
- `wsrvd` sidecar daemon and IPC protocol v0.
- PHP package: sidecar client and worker (pure PHP), embedded server over FFI (`Wsrv\Embedded\Server`) with the `authenticate` hook, echo and chat examples. Supports PHP 8.2 to the latest release (tested in CI).
- Composer package layout, `bin/wsrv-install-lib` (SHA-256 verified download of the native library) and a tag-triggered release workflow with checksums and build attestation (ADR-0026).
- Documentation set: requirements, architecture, ADRs, edge-case catalogue, 47 lessons.
- `wsrvd` container image (`docker/wsrvd.Dockerfile`, published to GHCR by `release.yml`, amd64 and arm64) and a Docker quickstart with a PHP worker (`examples/docker`), ADR-0026 amendment 1.
- Broker driven by connection id in the embedded server (`broker` and five `broker_max_*` config keys, ADR-0027 slice 1); the C ABI and PHP follow.

- Laravel integration guide (`docs/LARAVEL.md`), run against Laravel 13.
- PHP: `Connection::subscribe`/`unsubscribe`, `Worker::publish` (IPC 0.3) and `Wsrv\Publisher` for HTTP publishing (ADR-0028 slice 4); `wsrvd` reports the publish port.
- HTTP publish endpoint (`POST /publish`, bearer token file, bounded, off by default; `publish_port`, `publish_host`, `publish_token_file`), `wsrv_publish_http_total{status}`, ADR-0028 slice 3.
- IPC protocol 0.3: `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH` frames and the status `LIMIT`; the daemon drops a departed application's subscriptions at once (ADR-0028 slice 2).

### Fixed
- permessage-deflate: an offer that repeats a parameter is declined (RFC 7692 section 7) instead of failing the handshake; a message ended by a final block is accepted and keeps its window, with one restart per message.
