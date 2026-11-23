# Docker quickstart: wsrvd and a PHP worker

No compiler, no C++. Two containers: the `wsrvd` daemon (owns every WebSocket) and a PHP worker (reacts to events).

```sh
cd examples/docker
docker compose up -d           # pulls the published wsrvd image, builds the small PHP worker
```

Connect any WebSocket client to `ws://localhost:8080/` and send a message; the worker echoes it:

```sh
npx wscat -c ws://localhost:8080/
> hello
< hello
```

## Rooms and publishing

The same compose file runs the broker and the HTTP publish endpoint (ADR-0028). Join a room from one terminal, publish from another:

```sh
npx wscat -c ws://localhost:8080/
> join room.1
< joined room.1

# in a second terminal: a separate short-lived process publishes, as a controller or a queue job would
docker compose run --rm worker php /app/publish.php room.1 "hello from outside"
subscribers=1 accepted=1 refused=0
```

The first terminal prints `hello from outside`. Publishing needs the token in `/run/wsrv/publish.token`, which the one-shot `token-init` service creates in the shared volume; port 9200 is `expose:`d to the other containers and never published to your host. To publish from your own app container, give it that volume and `WSRV_PUBLISH_URL=http://wsrvd:9200`, then use `Wsrv\Publisher` (see [docs/LARAVEL.md](../../docs/LARAVEL.md)).

## What you are looking at
- `compose.yaml`: if port 8080 is busy, run `WSRV_PORT=18080 docker compose up --build`. Both services share the volume `/run/wsrv`, the daemon's private IPC socket directory.
- `worker/worker.php`: the whole PHP side (echo, and `join <room>`); `worker/publish.php`: publishing from another process.
- **Both containers run as uid 10001.** The IPC socket is mode `0600` and the daemon refuses a peer with a different uid (ADR-0017). If you swap in your own PHP image, keep `user: "10001:10001"` on it.
- `--allow-missing-origin=true` lets `wscat` connect (it sends no `Origin` header). Remove it for a browser-facing deployment and list your site with `--allow-origin=https://your.site`.

Production notes (TLS, token file, limits, metrics): [docs/OPERATIONS.md](../../docs/OPERATIONS.md) section 7. Image details: [ADR-0026 amendment 1](../../docs/adr/0026-php-package-and-native-library-distribution.md).
