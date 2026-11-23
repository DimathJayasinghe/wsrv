# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: [Semantic Versioning](https://semver.org/) for the C++ API and the PHP package; the C ABI has its own `major.minor` (`wsrv_abi_version()`), additive within a major.

Known gaps are listed in [docs/RELEASE.md](docs/RELEASE.md). `0.1.0-rc.1` and `0.1.0-rc.2` were early pre-releases of what is below.

## [Unreleased]

What the first stable release will contain. The owner decided (2026-11-23) that nothing is announced before the library is production-ready, so the first stable version is `1.0.0`, reached through `1.0.0-rc.N` pre-releases.

### Added
- The real client address behind a reverse proxy: `trusted_proxy` (addresses/CIDRs) and `client_ip_header` keys; the per-IP limits and the peer address in events then follow the client, not the proxy. Header ignored from untrusted peers, rightmost untrusted `X-Forwarded-For` entry, anything unusable falls back to the proxy address (ADR-0029). PROXY protocol is not implemented yet.
- Container and production operations (ADR-0030): every setting is a `WSRVD_<KEY>` environment variable (the base layer; flags and `--config` override it; an unknown `WSRVD_*` variable is a startup error), `wsrvd --healthcheck` (also the image's `HEALTHCHECK`), image defaults by environment, a hardened systemd unit and Kubernetes reference manifests under `deploy/`, and `docs/DEPLOYMENT.md`.
- Restarts are survivable (ADR-0031): `Wsrv\Supervisor` (the PHP worker reconnects to the daemon with exponential backoff and jitter, retries `BACKPRESSURE`, fails fast on a wrong token, reports a throwing handler without dropping the link, stops on `SIGTERM`), `Wsrv\ClusterPublisher` (publish to every daemon of a multi-node deployment, per-node failures reported) and `clients/js` (`WsrvClient`: browser/Node 22+ client with backoff, a fresh ticket per attempt and rooms re-joined after every reconnect), tested against a real `wsrvd` that is killed and restarted under a connected client.
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
