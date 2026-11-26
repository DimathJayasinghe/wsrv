# wsrv against the PHP WebSocket servers people actually use

This directory is the benchmark behind the table in [docs/BENCHMARKS.md](../../docs/BENCHMARKS.md). It exists so you can **run it yourself**: numbers from the author's laptop are a hint, numbers from your hardware are evidence.

```sh
# once: build the daemon image, recording the commit it comes from (the report prints it)
docker build -f docker/wsrvd.Dockerfile --build-arg REVISION=$(git rev-parse --short HEAD) -t wsrvd:local .

# four CPUs
BENCH_MEMORY=2g python3 bench/compare/run.py --servers wsrv,workerman,swoole,reverb --repeats 3 --label my-run

# one CPU per server, wsrvd on one loop thread: the per-core comparison
BENCH_MEMORY=2g BENCH_SERVER_CPUS=2 WSRV_THREADS=1 python3 bench/compare/run.py \
  --servers wsrv,workerman,swoole,reverb --scenarios echo,fanout --repeats 3 --label my-run-1cpu
```

It needs Docker with Compose and builds everything it runs (a few minutes the first time; Reverb installs a fresh Laravel app). Results land in `bench/compare/results/my-run.json`.

## What is compared

| Contender | What runs | Notes |
|---|---|---|
| **wsrv** | `wsrvd` (C++ I/O, 2 loop threads) + one PHP 8.3 worker over a Unix socket | the worker is `servers/wsrv/worker.php`; it joins every connection to a room on open |
| **Workerman 5** | one process on the `event` extension (libevent) | the broadcast loops over `$worker->connections` |
| **Swoole** | one worker, the official `phpswoole/swoole` image | the broadcast loops over `$server->connections` |
| **Laravel Reverb** | a fresh Laravel app, `reverb:start`, `ev` extension | Pusher protocol; the broadcast is a signed REST call, as a Laravel app sends it |

All four do the same three things (the code is in `servers/`, about 15 lines each):

1. **echo**: reply to a message with the same bytes;
2. **join**: every connection is in the room `bench` (wsrv: `subscribe` on open; the others: the process's own connection table; Reverb: `pusher:subscribe`);
3. **broadcast**: one trigger message reaches every connection in the room.

Each contender is the configuration that needs no extra infrastructure: one PHP process, so a broadcast can reach everyone. A multi-process Workerman or Swoole needs Redis (or similar) to broadcast across processes; wsrv does it inside the daemon. Where a contender could be configured differently (more workers, a Redis backplane) it would be a different benchmark.

## The scenarios

| Scenario | What the load generator does | What is reported |
|---|---|---|
| **echo** | N connections, each sends one 64-byte message, waits for the echo, sends the next (closed loop), for 15 s after a 3 s warm-up | messages per second, latency p50/p99/p99.9 |
| **idle** | opens N connections (1 000 and 10 000), holds them 15 s, sends nothing | connections established, and the **server's memory** (all its containers, sampled during the hold) minus the memory before |
| **fanout** | N subscribers; one trigger per round; 30 rounds; 128-byte message | time from the trigger to the **last** subscriber (p50/p99), and the ratio delivered (anything below 100% is shown) |

Every scenario is repeated 3 times; the table reports the median of the runs.

## Equal footing, and what is not equal

- Every container of a contender is pinned to the same four CPUs (`BENCH_SERVER_CPUS`) and has the same memory limit; the load generator runs on other CPUs, inside the same Docker network, so no host proxy sits between them.
- Release builds only (`wsrvd` from the release image build; Swoole, Workerman, Reverb as their images ship them). No debug extension, no profiler, no sanitizer.
- Per-IP connection limits of `wsrvd` are off (one load generator is one address); they are on by default in production, and the other servers have no such limit.
- **Not equal:** a single-process server cannot use four CPUs, `wsrvd` can (two loops plus the PHP worker). That is a real difference in how they scale, but it is not "faster per core". CPU use is not recorded, so the main run cannot say per-core efficiency; the `-1cpu` run pins every contender to one CPU (and `wsrvd` to one loop thread) to answer that.
- Message sizes, connection counts and durations are small compared with a production incident. They answer "how do these behave on the same hardware", not "how many users can I serve".

## Honest limits

- One machine, one run per label. Docker Desktop runs containers in a virtual machine: absolute numbers are lower than on bare Linux, and a busy laptop adds noise. Compare columns **within** a run; do not compare numbers between machines.
- The load generator (`wsbench/wsbench.cpp`) is ours. It shares no code with `wsrv`, speaks RFC 6455 only, and is the same for every contender, but it is still written by the project that benefits. Read it; it is one file.
- Reverb is a Pusher-protocol server with channels, presence and authentication the others do not have. The comparison is the work all four do (accept, echo-or-broadcast), not the whole feature set.
- Nothing here measures TLS, permessage-deflate, authentication, a database behind the handlers, or a WAN.
