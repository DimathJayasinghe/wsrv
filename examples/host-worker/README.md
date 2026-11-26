# A worker on your Mac or Windows PC, the daemon in Docker

`wsrvd` runs on Linux only. On macOS or Windows you run it in Docker Desktop (or WSL2). Before wsrv 1.1 your PHP worker had to run in a container too, because the IPC link was a Unix socket and a Unix socket cannot cross from your machine into Docker's Linux virtual machine. Now the link can be TCP, so the worker runs on the host, where your editor, debugger and code are.

```sh
cd examples/host-worker
docker compose up -d
export WSRV_TOKEN="$(docker compose run --rm --no-deps --entrypoint cat token-init /run/wsrv/ipc.token)"
composer require "wsrv/client:^1.1"   # PHP 8.2 or newer on the host; 1.0 cannot speak TCP
php worker.php
```

On Windows PowerShell:

```powershell
$env:WSRV_TOKEN = docker compose run --rm --no-deps --entrypoint cat token-init /run/wsrv/ipc.token
php worker.php
```

Connect a WebSocket client to `ws://localhost:8080/` and send a message: the worker echoes it.

## What to know

- **The link is not encrypted.** The compose file publishes the IPC port on your machine's loopback only (`127.0.0.1:9300`). Keep it that way, or put it behind a VPN or an SSH tunnel; never on an open network. A TCP peer has no operating-system identity, so the token (at least 32 bytes, in the volume) is the only authentication, and anyone who can reach the port can try it.
- **The daemon logs a warning, `ipc_plaintext`, and that is expected here.** Inside the container it must listen on `0.0.0.0` so that the published port arrives; the warning is there so that nobody does the same on a server by accident.
- **Why a volume and not a file from your drive.** The daemon refuses a token file that other users can read or another user owns (docs/OPERATIONS.md section 7). Docker Desktop shows a file mounted from a Windows or macOS drive as root-owned and world-readable, so the compose file creates the token inside a volume with the right owner and mode instead of loosening the rule.
- **Only one application at a time.** A second worker is refused with `BACKPRESSURE`; stop the first one before starting another.
- This is for development. In production, run the worker beside the daemon on Linux with a Unix socket (docs/DEPLOYMENT.md).
