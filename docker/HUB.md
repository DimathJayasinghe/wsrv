# wsrvd: a WebSocket server for PHP (and anything else) that you run as a container

`wsrvd` is the daemon of [wsrv](https://github.com/DimathJayasinghe/wsrv): a from-scratch C++23 implementation of RFC 6455 that owns your WebSocket connections, so your PHP (or other) application never has to hold a socket open. Your code connects to the daemon over a Unix socket and only reacts to events: *a browser asked to connect*, *a message arrived*, *a connection closed*.

Autobahn|Testsuite clean (517 cases, 0 failed, also over `wss://`), TLS, permessage-deflate, per-IP and global limits, a pub/sub broker (rooms), Prometheus metrics, health checks. Linux, `amd64` and `arm64`.

## Run it

```sh
docker run -d --name wsrvd -p 8080:8080 \
  -v wsrv-ipc:/run/wsrv \
  -e WSRVD_ALLOW_ORIGIN=https://app.example.com \
  -e WSRVD_BROKER=true \
  dimathjayasinghe/wsrvd
```

Then connect your application: `composer require wsrv/client` and

```php
$worker = new Wsrv\Worker(Wsrv\Client::connect('/run/wsrv/ipc.sock'));
$worker->on('message', fn ($conn, $data) => $conn->send($data));
$worker->run();
```

A complete Docker Compose stack with a PHP worker and a publish example is in [examples/docker](https://github.com/DimathJayasinghe/wsrv/tree/main/examples/docker); a Laravel guide is in [docs/LARAVEL.md](https://github.com/DimathJayasinghe/wsrv/blob/main/docs/LARAVEL.md).

## Configuration

Every setting is an environment variable: `WSRVD_` plus the key in upper case (`WSRVD_MAX_CONNECTIONS_PER_IP`, `WSRVD_LISTEN_PORT`, `WSRVD_TRUSTED_PROXY`, ...). Command-line flags and `--config=FILE` still work and override the environment. A mistyped `WSRVD_` variable stops the start and names itself instead of starting a weaker server. All keys: [docs/OPERATIONS.md](https://github.com/DimathJayasinghe/wsrv/blob/main/docs/OPERATIONS.md).

Defaults are safe: `Origin` is checked, per-IP limits are on, the admin port (`/healthz`, `/readyz`, `/metrics`) is bound to the container's loopback, and the container runs as an unprivileged user (uid 10001). The image has a `HEALTHCHECK`.

Behind a reverse proxy, set `WSRVD_TRUSTED_PROXY` to your proxy so limits and logs see the real client address, and read [docs/DEPLOYMENT.md](https://github.com/DimathJayasinghe/wsrv/blob/main/docs/DEPLOYMENT.md) (systemd unit, Kubernetes manifests, sizing, upgrading).

## Tags

`X.Y.Z` is an exact release, `X.Y.Z-rc.N` a release candidate, `latest` the newest stable release. Pin an exact version in production.

## Verify what you pull

Images are scanned before they are pushed, signed with cosign (keyless) and carry build provenance; each release has an SBOM.

Use cosign **3 or newer**: it signs and reads signatures in the Sigstore bundle format, and cosign 2.x answers `no signatures found` for these images.

```sh
cosign verify dimathjayasinghe/wsrvd:X.Y.Z \
  --certificate-identity-regexp 'https://github.com/DimathJayasinghe/wsrv/\.github/workflows/release\.yml@refs/tags/v.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

Source, issues, security reports: https://github.com/DimathJayasinghe/wsrv. License: Apache-2.0.
