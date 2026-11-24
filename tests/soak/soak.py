#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Soak and chaos run of the wsrvd release image.

    python3 tests/soak/soak.py --image ghcr.io/dimathjayasinghe/wsrvd:1.0.0-rc.1 --hours 2 --clients 200

What it does: starts wsrvd (the image under test), a supervised PHP worker and a fleet of reconnecting
JavaScript clients (tests/soak/compose.yaml); lets the fleet warm up; then, every 20 to 60 seconds, breaks
something at random (restart the daemon, restart the worker, kill the worker, restart the daemon with
SIGKILL, cut the clients' network for a few seconds); then stops breaking things, lets it settle and checks
what a production operator would check:

  - every client is connected again and gets an echo (the system recovers, nothing stays stuck);
  - the daemon's descriptors are back to the warm-up level (nothing leaks across restarts);
  - the daemon's memory did not grow more than 25% between warm-up and the end;
  - the daemon never crashed by itself (no OOM kill, no abort or terminate in its log).

It writes tests/soak/results/<label>.json with the samples. Exit code 0 only if every check holds.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import random
import re
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
PROJECT = "wsrv-soak"
COMPOSE = ["docker", "compose", "-f", str(HERE / "compose.yaml")]


def sh(cmd: list[str], check: bool = False) -> subprocess.CompletedProcess:
    done = subprocess.run(cmd, text=True, capture_output=True)
    if check and done.returncode != 0:
        raise SystemExit(f"{' '.join(cmd)} failed:\n{done.stderr}")
    return done


def cid(service: str) -> str:
    # -a: a killed container is not "running", and `ps` without it would not list it (an empty id
    # makes every later `docker start` a silent no-op)
    return sh(COMPOSE + ["ps", "-a", "-q", service]).stdout.strip()


def running(container: str) -> bool:
    return bool(container) and sh(["docker", "inspect", "-f", "{{.State.Running}}", container]).stdout.strip() == "true"


def mem_mb(container: str) -> float:
    out = sh(["docker", "stats", "--no-stream", "--format", "{{.MemUsage}}", container]).stdout
    m = re.match(r"\s*([\d.]+)\s*([KMG]i?B)", out)
    if not m:
        return 0.0
    return float(m.group(1)) * {"KiB": 1 / 1024, "MiB": 1.0, "GiB": 1024.0}[m.group(2)]


def fds(container: str) -> int:
    out = sh(["docker", "exec", container, "sh", "-c", "ls /proc/1/fd | wc -l"]).stdout.strip()
    return int(out) if out.isdigit() else -1


def crashed(container: str) -> bool:
    """A crash the script did not cause: the OOM killer, or an abort/terminate message in the daemon's log."""
    state = sh(["docker", "inspect", "-f", "{{.State.OOMKilled}}", container]).stdout.strip()
    logs = sh(["docker", "logs", container])
    text = logs.stdout + logs.stderr
    return state == "true" or any(w in text for w in ("terminate called", "Aborted", "core dumped", "AddressSanitizer"))


def last_tick(logs: str) -> dict | None:
    for line in reversed(logs.splitlines()):
        line = line.strip()
        if line.startswith("{") and '"kind"' in line:
            try:
                return json.loads(line)
            except ValueError:
                continue
    return None


# ---- chaos actions: each returns a short description of what it did -----------------------------

def action_restart_daemon() -> str:
    sh(["docker", "restart", "-t", "5", cid("wsrvd")], check=True)
    return "restart wsrvd (SIGTERM, graceful)"


def action_kill_daemon() -> str:
    daemon = cid("wsrvd")
    sh(["docker", "kill", "-s", "KILL", daemon])
    time.sleep(1)
    sh(["docker", "start", daemon], check=True)
    return "kill -9 wsrvd, start again"


def action_restart_worker() -> str:
    sh(["docker", "restart", "-t", "5", cid("worker")], check=True)
    return "restart the worker (SIGTERM)"


def action_kill_worker() -> str:
    worker = cid("worker")
    sh(["docker", "kill", "-s", "KILL", worker])
    time.sleep(1)
    sh(["docker", "start", worker], check=True)
    return "kill -9 the worker, start again"


def action_cut_network() -> str:
    network = f"{PROJECT}_default"
    seconds = random.randint(3, 10)
    load = cid("load")
    sh(["docker", "network", "disconnect", network, load])
    time.sleep(seconds)
    sh(["docker", "network", "connect", "--alias", "load", network, load])
    return f"cut the clients' network for {seconds} s"


ACTIONS = [action_restart_daemon, action_kill_daemon, action_restart_worker, action_kill_worker, action_cut_network]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True, help="the wsrvd image under test (e.g. ghcr.io/...:1.0.0-rc.1)")
    ap.add_argument("--hours", type=float, default=1.0, help="how long to keep breaking things")
    ap.add_argument("--clients", type=int, default=200)
    ap.add_argument("--warmup", type=int, default=120, help="seconds before the first fault")
    ap.add_argument("--min-gap", type=int, default=20)
    ap.add_argument("--max-gap", type=int, default=60)
    ap.add_argument("--label", default=datetime.date.today().isoformat())
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    random.seed(seed)

    env = dict(os.environ, WSRVD_IMAGE=args.image, SOAK_CLIENTS=str(args.clients))
    os.environ.update(env)
    print(f"image {args.image}, {args.clients} clients, {args.hours} h of faults, seed {seed}", flush=True)
    sh(COMPOSE + ["down", "-v", "--remove-orphans"])
    sh(COMPOSE + ["up", "-d"], check=True)

    samples: list[dict] = []
    faults: list[dict] = []

    def sample(tag: str) -> dict:
        logs = sh(["docker", "logs", "--tail", "200", cid("load")]).stdout
        tick = last_tick(logs) or {}
        row = {"t": round(time.time() - t0), "tag": tag, "daemon_mb": round(mem_mb(cid("wsrvd")), 1),
               "worker_mb": round(mem_mb(cid("worker")), 1), "daemon_fds": fds(cid("wsrvd")),
               "open": tick.get("open"), "echoed": tick.get("echoed"), "lost": tick.get("lost"),
               "reconnects": tick.get("reconnects"), "p99_ms": tick.get("p99_ms"),
               "states": tick.get("states"), "stuck": tick.get("stuck")}
        samples.append(row)
        print("  " + " ".join(f"{k}={v}" for k, v in row.items()), flush=True)
        return row

    t0 = time.time()
    try:
        time.sleep(args.warmup)
        warm = sample("warm-up")
        end = time.time() + args.hours * 3600
        next_sample = time.time()
        while time.time() < end:
            action = random.choice(ACTIONS)
            started = time.time() - t0
            what = action()
            faults.append({"t": round(started), "what": what})
            print(f"[{round(started):>6} s] {what}", flush=True)
            gap = random.randint(args.min_gap, args.max_gap)
            stop = time.time() + gap
            while time.time() < stop:
                time.sleep(5)
                if time.time() >= next_sample:
                    sample("during")
                    next_sample = time.time() + 120
        print("faults over: settling for 90 s", flush=True)
        time.sleep(90)
        final = sample("settled")

        # A last proof of life: stop the clients and read their totals.
        sh(["docker", "kill", "-s", "TERM", cid("load")])
        time.sleep(2)
        logs = sh(["docker", "logs", "--tail", "50", cid("load")]).stdout
        totals = last_tick(logs) or {}
        # `open` in the last tick before the stop is the number connected after the settle period.
        checks = {
            "all clients connected after settling": final["open"] == args.clients,
            "descriptors back to the warm-up level (+20)": 0 <= final["daemon_fds"] <= warm["daemon_fds"] + 20,
            "daemon memory within 25% of warm-up": final["daemon_mb"] <= warm["daemon_mb"] * 1.25 + 5,
            "no echo arrived that was never asked for": totals.get("bad_echo", 0) == 0,
            "the daemon and the worker are running": running(cid("wsrvd")) and running(cid("worker")),
            "the daemon never crashed by itself": running(cid("wsrvd")) and not crashed(cid("wsrvd")),
        }
        ok = all(checks.values())
    finally:
        out = {"label": args.label, "image": args.image, "seed": seed, "clients": args.clients,
               "hours": args.hours, "faults": faults, "samples": samples}
        try:
            out["checks"] = checks  # noqa: F821 - set above when the run got that far
            out["totals"] = totals  # noqa: F821
        except NameError:
            out["checks"] = {"run completed": False}
        results = HERE / "results"
        results.mkdir(exist_ok=True)
        (results / f"{args.label}.json").write_text(json.dumps(out, indent=2) + "\n")
        # What the containers said, kept next to the numbers: a failed run is only useful if it can be explained.
        logs = sh(COMPOSE + ["logs", "--no-color", "--tail", "400", "wsrvd", "worker"])
        (results / f"{args.label}.logs.txt").write_text(logs.stdout + logs.stderr)
        sh(COMPOSE + ["down", "-v", "--remove-orphans"])

    print(json.dumps(checks, indent=2))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
