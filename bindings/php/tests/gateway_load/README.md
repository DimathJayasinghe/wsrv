# Gateway load and churn runs

The unit tests say each rule of the Pusher gateway is right. These runs say what a real daemon, a real
worker process and thousands of browsers do to it together: how fast it joins, where it saturates, and, the
check that matters most for a long-running process, whether **everything it held is gone again** when the
browsers are.

```
load.mjs   a fleet of Pusher-protocol browsers (Node's built-in WebSocket, no dependency)
worker.php the code `php artisan wsrv:echo` runs (Supervisor, Gateway::forWorker, the statistics line), without Laravel
run.py     starts wsrvd, worker.php and load.mjs pinned to separate cores, samples CPU and memory, checks the end state
```

Needs `node` 22.4 or newer (`WebSocket` is global), `php` 8.2 or newer, `python3`, and a `wsrvd` binary: a
**release build** for numbers (`cmake --preset release && cmake --build --preset release --target wsrvd`).

## Scenarios

```
# A reconnect storm: 5000 browsers connect at once, each joins 2 public, 1 private and 1 presence channel
python3 run.py --daemon build/release/apps/wsrvd/wsrvd --scenario join --clients 5000 --procs 4 \
    --public 2 --private 1 --presence 1 --room-size 50 --hold 5

# Whispers: 400 private channels with 5 members each, every member whispers 10 times a second
python3 run.py --daemon ... --scenario whisper --members 5 --groups 400 --rate 10 --duration 20 --procs 8

# Churn: 200 loops of connect, join random channels, whisper, leave, disconnect (a sixth of them before
# the daemon has answered), 5% of the connections abusive, a load process killed with SIGKILL every 12 s
python3 run.py --daemon ... --scenario churn --concurrency 200 --duration 120 --procs 4 --kill-every 12
```

Each run prints one JSON summary and writes `results/<label>.json` plus the worker's and the daemon's logs
(not committed). `--options '{"clientEventsPerSecond":100,"messagesPerSecond":200,"messageBurst":400}'` passes
named `GatewayConfig` arguments to the worker, for a run that is about the gateway's ceiling and not about its
limits. `--broker-topics` and `--broker-subscriptions` set the daemon's broker bounds; the runs raise them by
default (see the Echo guide, "Deploying": the daemon's defaults are small for a site with a private channel per user).

## What the run checks

The exit code is 0 only if all of these hold:

- no load process failed or had to be killed for running past its time limit;
- every `pusher:subscribe` was answered (a join the gateway loses shows up as a *subscribe timeout*, counted,
  and never as a hang);
- `join`: every connect and every subscribe succeeded, and no browser was lost during the hold;
- **after the last browser has gone, the gateway's statistics line shows `connections`, `channels`,
  `joins_in_progress`, `presence_channels` and `presence_members` all at 0** (the run waits up to `--settle`
  seconds for that). A gauge that stays above zero is a leak in the gateway: state it keeps for browsers that no
  longer exist.

It also reports, for you to read: CPU of the daemon and of the worker (percent of one core), frames and joins
per second, the worker's resident memory and PHP's own `memory_get_usage()` over the run (it should reach a
plateau, not climb), and, for the whisper scenario, delivery loss and latency.

## Reading the numbers honestly

Everything runs on one machine, over loopback, with the three parts pinned to different cores (`--cores-daemon`,
`--cores-worker`, `--cores-clients`), so the load generator does not steal the cores it is measuring. That says
how much one PHP process and a two-thread daemon can do; it says nothing about a network, TLS, a proxy or your
application's own `/broadcasting/auth` route, which every private and presence join also costs.
PHP's `memory_get_usage()` does not fall back to its starting value after a big run, because PHP arrays keep the
capacity they grew to: compare the plateau of a second cycle with the first, as the churn scenario does, not the end
with the beginning.
