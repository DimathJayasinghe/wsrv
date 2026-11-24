# Benchmarks: wsrv against the PHP WebSocket servers people use

wsrv is meant to be the WebSocket server a PHP developer reaches for, so it is measured against what
that developer would otherwise run: **Workerman 5**, **Swoole** and **Laravel Reverb**. This page
shows the numbers, what they do and do not mean, and the three bugs the benchmark found in wsrv
itself. The harness is in [`bench/compare/`](../bench/compare/README.md); you can run it on your own
machine, and you should: these numbers are one laptop's, not a promise.

## The short version

- **With four CPUs, wsrv is ahead.** Echo: 1.5 to 1.8 times the messages per second of Workerman and
  Swoole with 64 connections (182 000 against 101 000 and 125 000), and 1.7 to 1.9 times with 512
  (192 000 against 101 000 and 113 000). A broadcast reaches the last of 5 000 subscribers in about
  half the time (31 ms against 52 to 59 ms).
- **Per core, wsrv is behind on echo, by about 10 to 25 percent.** Pinned to a single CPU, with 512
  connections wsrv does 99 000 messages per second against 112 000 (Workerman, 13 percent more) and
  126 000 (Swoole, 27 percent more); with 64 connections 100 000 against 120 000 and 135 000. The
  daemon and the PHP worker are two processes and every message crosses a Unix socket twice; the
  others do the same work in one process. wsrv's lead above comes from using more cores, not from
  doing less work per message. This benchmark found two causes of a much larger gap (35 to 46 percent
  behind at first): a host wake-up per message and two system calls per message that carried no
  information; both are fixed (below). A broadcast is a tie at one CPU: 8.6 ms against 8.4 to 9.7 ms
  to reach 1 000 subscribers, and 40.9 ms against 40.7 to 44.4 ms at 5 000.
- **Memory is a tie with the single-process servers** at 8 to 9 KiB per idle connection (10 000
  connections), and wsrv has the larger fixed base (two processes). **Reverb uses 2.5 times as much**
  (about 20 KiB per connection).
- **Not measured:** TLS, permessage-deflate, authentication, a database behind the handlers, a WAN,
  CPU use, and Workerman or Swoole running several worker processes (which then need Redis or similar
  to broadcast across processes).

## What is compared

| Contender | What runs |
|---|---|
| wsrv | `wsrvd` (C++ I/O, two loop threads by default) and one PHP 8.3 worker connected over a Unix socket |
| Workerman 5 | one process on the `event` extension |
| Swoole | one worker, the official `phpswoole/swoole` image (6.3.0RC2: a release candidate) |
| Laravel Reverb | a fresh Laravel app with `reverb:start`; the Pusher protocol, whose broadcast is a signed REST call |

Each does the same three things (about 15 lines of PHP each, in
[`bench/compare/servers/`](../bench/compare/servers/)): **echo** a message, put every connection in
one room, and **broadcast** one message to the whole room. Every contender is the configuration that
needs no extra infrastructure, so a broadcast can reach everybody.

The load generator, `wsbench`, is a single C++ file in the same directory. It is ours. It speaks only
RFC 6455, is identical for every contender, and the comparison is only as honest as it is: read it.

## Four CPUs

Every container of a contender is pinned to the same four CPUs and given the same 2 GiB memory limit each; the load
generator runs on other CPUs in the same Docker network, so no proxy sits between them. `wsrvd` runs
with its default two loop threads.

_Run `2026-11-24-4cpu` on 20 CPUs, 3895296000 bytes, Docker Desktop, kernel 6.12.76-linuxkit; median of 3 runs._

### Echo (closed loop, 64-byte messages; Reverb is not here: the Pusher protocol has no echo)

| Server | Connections | Messages/s | p50 (ms) | p99 (ms) |
|---|---|---|---|---|
| wsrv (wsrvd + PHP worker) | 64 | 181,952 (178,844 to 188,565) | 0.33 | 0.65 |
| Workerman 5 (event) | 64 | 101,426 (99,922 to 106,706) | 0.62 | 1.22 |
| Swoole | 64 | 124,595 (116,502 to 126,108) | 0.49 | 1.08 |
| wsrv (wsrvd + PHP worker) | 512 | 192,036 (190,747 to 202,415) | 2.61 | 5.02 |
| Workerman 5 (event) | 512 | 100,582 (91,238 to 107,505) | 4.84 | 7.92 |
| Swoole | 512 | 112,610 (111,571 to 118,367) | 4.46 | 6.79 |

### Idle connections (memory of all the server's containers)

| Server | Connections | Established | Before | Peak | Per connection |
|---|---|---|---|---|---|
| wsrv (wsrvd + PHP worker) | 1000 | 1000 | 12 MB | 18 MB | 6.0 KiB |
| Workerman 5 (event) | 1000 | 1000 | 11 MB | 18 MB | 6.1 KiB |
| Swoole | 1000 | 1000 | 10 MB | 21 MB | 10.4 KiB |
| Laravel Reverb | 1000 | 1000 | 37 MB | 59 MB | 21.8 KiB |
| wsrv (wsrvd + PHP worker) | 10000 | 10000 | 14 MB | 99 MB | 8.3 KiB |
| Workerman 5 (event) | 10000 | 10000 | 14 MB | 104 MB | 8.8 KiB |
| Swoole | 10000 | 10000 | 11 MB | 102 MB | 8.9 KiB |
| Laravel Reverb | 10000 | 10000 | 56 MB | 266 MB | 20.5 KiB |

### Fan-out (one broadcast to every subscriber, 128 bytes)

| Server | Subscribers | To the last (p50, ms) | To the last (p99, ms) | Delivered |
|---|---|---|---|---|
| wsrv (wsrvd + PHP worker) | 1000 | 7.0 (6.7 to 7.1) | 13.0 | 100.00 % |
| Workerman 5 (event) | 1000 | 12.3 (12.2 to 12.6) | 22.7 | 100.00 % |
| Swoole | 1000 | 11.1 (10.8 to 11.4) | 20.2 | 100.00 % |
| Laravel Reverb | 1000 | 21.2 (21.0 to 21.5) | 40.6 | 100.00 % |
| wsrv (wsrvd + PHP worker) | 5000 | 31.1 (30.6 to 31.1) | 39.5 | 100.00 % |
| Workerman 5 (event) | 5000 | 58.6 (58.6 to 60.3) | 74.5 | 100.00 % |
| Swoole | 5000 | 52.2 (51.1 to 54.1) | 68.5 | 100.00 % |
| Laravel Reverb | 5000 | 89.5 (88.6 to 96.0) | 116.0 | 100.00 % |

## One CPU

The same run with all of a contender's containers pinned to a **single** CPU and `wsrvd` on one loop
thread. This is the per-core comparison: the single-process servers cannot use more than one core in
either run, so this is where they are at their best. The idle-memory scenario is not repeated: it does
not depend on the CPU count.

_Run `2026-11-24-1cpu` on 20 CPUs, 3895296000 bytes, Docker Desktop, kernel 6.12.76-linuxkit; median of 3 runs._

### Echo (closed loop, 64-byte messages; Reverb is not here: the Pusher protocol has no echo)

| Server | Connections | Messages/s | p50 (ms) | p99 (ms) |
|---|---|---|---|---|
| wsrv (wsrvd + PHP worker) | 64 | 100,428 (95,973 to 101,696) | 0.63 | 0.90 |
| Workerman 5 (event) | 64 | 120,234 (118,859 to 124,326) | 0.52 | 1.01 |
| Swoole | 64 | 134,664 (131,453 to 139,158) | 0.46 | 0.92 |
| wsrv (wsrvd + PHP worker) | 512 | 99,287 (97,096 to 99,953) | 5.08 | 7.04 |
| Workerman 5 (event) | 512 | 112,135 (109,946 to 114,581) | 4.50 | 6.49 |
| Swoole | 512 | 126,032 (124,971 to 135,031) | 3.99 | 5.67 |

### Fan-out (one broadcast to every subscriber, 128 bytes)

| Server | Subscribers | To the last (p50, ms) | To the last (p99, ms) | Delivered |
|---|---|---|---|---|
| wsrv (wsrvd + PHP worker) | 1000 | 8.6 (8.6 to 8.8) | 14.9 | 100.00 % |
| Workerman 5 (event) | 1000 | 9.7 (9.5 to 10.2) | 19.6 | 100.00 % |
| Swoole | 1000 | 8.4 (8.3 to 8.7) | 12.3 | 100.00 % |
| Laravel Reverb | 1000 | 16.3 (13.8 to 18.4) | 41.0 | 100.00 % |
| wsrv (wsrvd + PHP worker) | 5000 | 40.9 (40.8 to 41.0) | 52.5 | 100.00 % |
| Workerman 5 (event) | 5000 | 44.4 (44.1 to 47.3) | 64.3 | 100.00 % |
| Swoole | 5000 | 40.7 (39.7 to 41.5) | 51.7 | 100.00 % |
| Laravel Reverb | 5000 | 92.1 (84.6 to 93.9) | 109.7 | 100.00 % |

## Reading the numbers

- **Noise is large.** The runs are on Docker Desktop (a virtual machine on a laptop), so a number can
  move by 10 to 20 percent between runs. The ranges in the tables are the lowest and highest of three
  runs; wsrv's echo with 64 connections ranged from about 179 000 to 189 000 messages per second.
  Treat differences under 10 to 15 percent as a tie. Compare columns of one run with each other;
  do not compare these numbers with numbers from another machine. Across days it is worse: Workerman's
  four-CPU echo with 64 connections was 127 000 on 2026-11-23 and 101 000 on 2026-11-24 on the same
  laptop. A first four-CPU run on the 24th, with every server about
  four times slower, was discarded and repeated on a quiet machine.
- **Echo is a closed loop**: each connection waits for its answer before sending the next message, so a
  slow server makes the generator slower too. The latencies are service times at that load, not what a
  user sees when a server is overloaded (coordinated omission is not corrected).
- **A broadcast time is the time to the last subscriber**, which is what a chat room feels like. Every
  row delivered 100 percent of the messages; a lower ratio would be shown.
- **"Per connection" memory** is the peak memory of all the server's containers during a 15-second
  hold, minus the memory before, divided by the connections established. It is the cost of holding a
  connection, not the cost of serving one.
- **Why four CPUs help wsrv and not the others:** the daemon's loop threads and the PHP worker run on
  different cores, while a single-process server is capped at one. That is a real difference in how
  they scale, but it is not "faster per core", and the one-CPU table shows the cost of the extra hop.
  The echo gap at one CPU is what is left of that hop after the two fixes below; the remaining time
  is the second process and the kernel's socket work.

## What the benchmark found in wsrv

Running thousands of connections found three bugs that no unit test had reached, and the one-CPU
comparison found a performance problem. All are fixed and the numbers above are from after the fixes:

1. **At most 4 095 concurrent connections.** The host event queue held 4 096 events and every live
   connection reserves a slot for its Close event. The queue now follows `max_connections`
   ([ADR-0016 amendment A2](adr/0016-c-abi-v0-embedded-pull-model.md)).
2. **A broadcast lost recipients.** One queue slot per recipient meant one loop took at most 3 072
   recipients of a publish: with one loop thread a 5 000-subscriber room received 61 percent of every
   broadcast. A broadcast now takes one slot per 256 recipients ([ADR-0034](adr/0034-bulk-fan-out-in-chunks.md)).
3. **Connections left open after the worker died.** Closing 5 000 connections hit the same full queue
   and ignored the refusal: 904 stayed open for good. Refused closes are now retried every 25 ms
   ([ADR-0034](adr/0034-bulk-fan-out-in-chunks.md)).
4. **One host wake-up per message (performance).** With one CPU, the loop thread and the daemon's IPC
   thread took turns for every message: an eventfd write and read, a futex wake-up and two context
   switches each time, 914 000 switches in one run. The wake-up is now paid once per loop iteration
   ([ADR-0035](adr/0035-wake-the-host-once-per-loop-iteration.md), [lesson 49](learning/49-a-wake-up-per-message-and-how-to-find-it.md)),
   which took one-CPU echo from about 53 000 to about 83 000 messages per second on this laptop. Two
   more system calls per message that carried no information (a read that only answers "would
   block", and `getpid()` per call) went in [ADR-0036](adr/0036-two-fewer-system-calls-per-message.md),
   another 8 percent.

## Limits of this benchmark

- One machine, one virtual machine, three runs. No bare-metal Linux, no multi-host.
- The load generator is written by the project that benefits from the result.
- Small messages (64 and 128 bytes) over plain TCP on a loopback-like network. No TLS, no compression,
  no WAN latency, no application logic behind the handlers.
- A single PHP worker for wsrv, and a single process for Workerman and Swoole. Running several of each
  (and Redis for the cross-process broadcast) is a different benchmark.
- Reverb carries channels, presence and authentication the others do not. The comparison is the work
  all four do (accept, echo or broadcast), not the feature sets.
- Memory is measured with `docker stats`, which counts what the kernel charges to the containers.

## Reproduce

```sh
# once: build the daemon image, recording the commit it comes from
docker build -f docker/wsrvd.Dockerfile --build-arg REVISION=$(git rev-parse --short HEAD) -t wsrvd:local .

# four CPUs
BENCH_MEMORY=2g python3 bench/compare/run.py --servers wsrv,workerman,swoole,reverb --repeats 3 --label my-run

# one CPU
BENCH_MEMORY=2g BENCH_SERVER_CPUS=2 WSRV_THREADS=1 python3 bench/compare/run.py \
  --servers wsrv,workerman,swoole,reverb --scenarios echo,fanout --repeats 3 --label my-run-1cpu

python3 bench/compare/report.py bench/compare/results/my-run.json
```

The raw results of the runs on this page are in
[`bench/compare/results/`](../bench/compare/results/) (`2026-11-24-4cpu.json`, `2026-11-24-1cpu.json`; the 2026-11-23 runs are the ones before the fixes in item 4).
`BENCH_SERVER_CPUS` is the list of CPUs the servers may use (default `2-5`).

## What was run

- wsrv (wsrvd + PHP worker): wsrvd image wsrvd:local built from commit c460f79, PHP 8.3.37 in the worker
- Workerman 5 (event): Workerman 5.2.4, PHP 8.3.37
- Swoole: Swoole 6.3.0RC2, PHP 8.3.37
- Laravel Reverb: Laravel Reverb v1.14.0 (Laravel Framework 13.43.0), PHP 8.3.37

Docker: 20 CPUs, 3895296000 bytes, Docker Desktop, kernel 6.12.76-linuxkit.
