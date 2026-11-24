# Soak and chaos test

Runs the release image of `wsrvd`, a supervised PHP worker and a fleet of reconnecting JavaScript clients
(`clients/js`), and breaks the stack at random while the clients keep talking: graceful and `kill -9`
restarts of the daemon, restarts and kills of the worker, and the clients' network cut for a few seconds.
Then it stops breaking things, waits, and checks what an operator would check.

```sh
python3 tests/soak/soak.py --image ghcr.io/dimathjayasinghe/wsrvd:1.0.0-rc.1 --hours 2 --clients 200
python3 tests/soak/soak.py --image wsrvd:local --hours 0.06 --clients 50 --warmup 30 --min-gap 10 --max-gap 20 --label quick
```

Needs Docker with Compose. The load container is `node:22-slim` on purpose: it is the platform where the
WebSocket client misbehaves differently from a desktop Node (ADR-0031 A1). `SOAK_CLIENT_SRC=<dir>` runs a
client from another checkout.

| Check | Meaning |
|---|---|
| all clients connected after settling | the system recovers; nothing stays stuck |
| descriptors back to the warm-up level (+20) | nothing leaks across restarts |
| daemon memory within 25% of warm-up | no growth |
| no echo that was never asked for | a reply to a message nobody sent, or a second reply, is a bug |
| daemon and worker running, daemon never crashed | no OOM kill, abort or terminate |

A *late* echo (a reply that arrives after the client stopped waiting, or after it reconnected, for example
held back by a cut network) is counted as `late_echo` and is not an error. `lost` counts messages that got no
reply in 10 s or died with their connection: expected during faults, reported, not judged.

Results (`results/<label>.json`) and the container logs (`results/<label>.logs.txt`) are written next to the
script and are not committed. Exit code 0 only if every check holds.

## What it has found

- The JavaScript client never reconnected after one failed attempt on Node 22's built-in WebSocket (it fires
  `error` and never `close`). All 50 clients were stuck 5 minutes after one daemon restart. Fixed in
  ADR-0031 A1; with the fix every client is connected again after every fault.
