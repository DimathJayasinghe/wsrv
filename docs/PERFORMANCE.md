# Performance notes

Measured numbers, how to reproduce them, and what they do **not** show. Numbers come from one
development machine and are for comparing loop counts, not for sizing a deployment. Nothing here is
a speed gate (see "Regression gates" at the end).

## Tool

`tests/bench/load.cpp`, target `wsrv_load` (build with the `bench` preset: Release with LTO, benchmarks on).
It starts the echo server in its own process and runs blocking client threads against it over
loopback. Each client thread owns K connections and works in rounds: send on all, then read all.

```sh
cmake --preset bench && cmake --build --preset bench -j --target wsrv_load
./build/bench/tests/bench/wsrv_load --threads 4 --clients 32 --connections 8 --seconds 6 --size 64
```

Options: `--threads` (server loops), `--clients` (client threads), `--connections` (per client),
`--seconds`, `--size` (payload bytes), `--churn` (replace one connection per round), `--sample`
(soak: seconds between samples). Output: round trips per second, latency p50/p99 (send to echo,
measured per round, so it includes queueing behind the other connections of the same client
thread), failures.

## Scaling (2026-10-30, 64-byte text echo, 256 connections, 32 client threads)

Machine: 13th Gen Intel Core i7-13700H (20 logical CPUs, hybrid P and E cores), Docker Desktop on
Linux (the dev container runs in a VM), GCC Release with LTO. Two rounds of 6 s each.

| Loops | Round trips per second | p50 | p99 |
|------:|-----------------------:|----:|----:|
| 1 | 240 000 / 224 000 | 0.99 / 1.07 ms | 2.1 / 1.8 ms |
| 2 | 352 000 / 354 000 | 0.62 / 0.61 ms | 1.3 / 1.4 ms |
| 4 | 363 000 / 345 000 | 0.70 / 0.70 ms | 0.9 / 0.9 ms |
| 8 | 555 000 / 541 000 | 0.35 / 0.36 ms | 1.1 / 1.4 ms |

About 1.0 : 1.5 : 1.5 : 2.4 in throughput. A first run with 16 client threads gave the same shape
(272 k, 366 k, 362 k, 550 k).

## What this does not show

- **Client and server share the machine.** The 32 blocking client threads compete with the loops
  for the same cores, so the server is not the only thing being scaled. Per-core server cost needs
  a separate load machine.
- **The 4-loop result is not explained.** It is no better than 2 loops in every run. I did not
  investigate; likely candidates are thread placement on a hybrid CPU (E cores, hyper-threading
  siblings) and the client threads, not the server. Run again with `taskset` or on a homogeneous
  machine before drawing conclusions.
- Loopback inside a VM, one message size, plain TCP, no TLS, one 8-core-class laptop.
- Round-robin hand-off ignores load: a loop that happens to hold the busy connections is not
  relieved (ADR-0014).

## Soak

`wsrv_load --churn` replaces a connection every round and samples open descriptors and resident
memory. It fails (exit 1) if descriptors exceed the first sample by more than `4 x clients + 32`,
or resident memory exceeds the first sample by more than 25 % + 16 MiB. A first version of the
tool leaked by itself (it kept every latency sample): in churn mode the samples are now capped, and
a 70 s run showed a flat 7.5 MB and ~148 descriptors.

The M7 exit criterion is a one-hour run. Command and result:

```sh
wsrv_load --threads 4 --clients 4 --connections 8 --seconds 3600 --churn --sample 60
```

Status: finished 2026-10-30 (exit 0).

| Run | Result |
|-----|--------|
| 2026-10-30, 1 h, 4 loops, 32 churning connections (64-byte echoes, 60 samples, dev container, ASan/UBSan-free `bench` build) | 0 failures; 119,107 messages/s over the hour; latency p50 39 us, p99 104 us; open descriptors stayed between 78 and 86 (no growth); RSS peaked at 6.1 MiB early on, settled at 2.4 MiB and stayed flat for the rest of the hour (no leak trend). The soak ran while other builds were using the same machine, so the throughput is a floor, not a benchmark. |

## Pub/sub fan-out (2026-11-14, M10)

`tests/bench/fanout.cpp`, target `wsrv_fanout` (same `bench` preset). An in-process server with a
`Broker` (ADR-0024); N subscriber connections read by 8 blocking client threads; the main thread
publishes one text message, waits until every subscriber has read it, then repeats. A round time is
therefore "publish until the last subscriber has the message".

```sh
cmake --preset bench && cmake --build --preset bench -j --target wsrv_fanout
./build/bench/tests/bench/wsrv_fanout --threads 4 --subscribers 1000 --rounds 2000 --size 128
```

Same machine and build as the scaling table above (i7-13700H, Docker Desktop VM, GCC Release with
LTO), taken after the M10 review fixes, 2000 rounds, 128-byte payload unless stated, `refused` and
failures all 0:

| Loops | Subscribers | Deliveries per second | Round p50 | Round p99 |
|------:|------------:|----------------------:|----------:|----------:|
| 1 | 10 | 171 000 | 48 us | 153 us |
| 1 | 100 | 247 000 | 372 us | 798 us |
| 1 | 1000 | 212 000 | 4.3 ms | 9.9 ms |
| 4 | 10 | 193 000 | 49 us | 132 us |
| 4 | 100 | 493 000 | 199 us | 285 us |
| 4 | 1000 | 484 000 | 2.0 ms | 3.6 ms |
| 4 | 1000, 4 KiB payload | 243 000 | 4.0 ms | 7.4 ms |

**Read the shape, not the digits.** An earlier run of the same code before the review fixes gave
227 k, 166 k and 119 k deliveries per second for one loop at 10, 100 and 1000 subscribers, against
171 k, 247 k and 212 k here, so a single configuration varies by up to a factor of two between
runs on this machine (shared cores, a VM, 8 client threads doing the reading). What is stable:
a publish to 1000 subscribers on 4 loops reaches all of them in about 2 ms (one `post` per
subscriber, each loop copying the shared payload into its connections' queues); four loops deliver
about twice the rate of one at 100 and 1000 subscribers; with 10 subscribers the hand-off costs
about what it saves. The payload is shared (ADR-0024 section 3), so the larger message costs the
per-connection copy and the socket write, not an allocation per subscriber.

What it does **not** show: the clients are 8 threads that read their connections one after the
other, so the round time includes the reading side and the numbers are a floor for the server; one
publisher thread only (publishing from several threads contends on the registry mutex, which is held
for a table lookup and a copy of the handle list, not shown here); lockstep rounds, so queues never
fill (the refusal path is covered by `SmallQueueBrokerTest`, not measured); loopback in a VM; plain
TCP, no TLS, no compression.

## Regression gates

The two tools fail (exit 1) on **correctness**, not speed: `wsrv_load` on any failed echo and on a
leak in soak mode (descriptors, resident memory), `wsrv_fanout` on a failed read or any refused
delivery. There is no CI gate on throughput or latency: shared CI runners vary by more than the
differences that matter, and a flaky gate trains people to ignore it. The numbers above are the
baseline to compare against by hand on one machine before and after a change to the hot path
(`Connection`, `EventLoop`, `Broker`); a deployment that wants an automatic gate should run the tools
on a dedicated machine and keep their own thresholds.
