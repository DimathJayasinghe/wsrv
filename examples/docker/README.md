# Docker quickstart: wsrvd and a PHP worker

No compiler, no C++. Two containers: the `wsrvd` daemon (owns every WebSocket) and a PHP worker (reacts to events).

```sh
cd examples/docker
docker compose up --build      # builds the daemon image from this clone (no published image exists before the next tag)
```

Connect any WebSocket client to `ws://localhost:8080/` and send a message; the worker echoes it:

```sh
npx wscat -c ws://localhost:8080/
> hello
< hello
```

## What you are looking at
- `compose.yaml`: if port 8080 is busy, run `WSRV_PORT=18080 docker compose up --build`. Both services share the volume `/run/wsrv`, the daemon's private IPC socket directory.
- `worker/worker.php`: the whole PHP side, five lines (`Worker::on('message', ...)`).
- **Both containers run as uid 10001.** The IPC socket is mode `0600` and the daemon refuses a peer with a different uid (ADR-0017). If you swap in your own PHP image, keep `user: "10001:10001"` on it.
- `--allow-missing-origin=true` lets `wscat` connect (it sends no `Origin` header). Remove it for a browser-facing deployment and list your site with `--allow-origin=https://your.site`.

Production notes (TLS, token file, limits, metrics): [docs/OPERATIONS.md](../../docs/OPERATIONS.md) section 7. Image details: [ADR-0026 amendment 1](../../docs/adr/0026-php-package-and-native-library-distribution.md).
