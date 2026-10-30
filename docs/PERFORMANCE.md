# Performance notes

Measured numbers, how to reproduce them, and what they do **not** show. Numbers come from one
development machine and are for comparing loop counts, not for sizing a deployment. Nothing here is
a regression gate yet (that is M10).

## Tool

`tests/bench/load.cpp`, target `wsrv_load` (build with the `bench` preset: Release, benchmarks on).
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
