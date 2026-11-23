#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the wsrv comparison (bench/compare/README.md) and write the raw numbers as JSON plus a Markdown table.

    python3 bench/compare/run.py --servers wsrv,workerman,swoole,reverb --repeats 3 --label 2026-11-23

Needs Docker with Compose. One contender at a time is started from compose.yaml, every scenario is run by
the `loadgen` container on the same Docker network, and memory is read with `docker stats`. Nothing here
is a pass/fail check: it records what was measured, with the settings used, so a reader can judge it.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
COMPOSE = ["docker", "compose", "-f", str(HERE / "compose.yaml")]

# How each contender is started, reached and asked to broadcast (all equivalent, see README "What is compared").
SERVERS = {
    "wsrv": {
        "profile": "wsrv",
        "services": ["wsrvd", "worker"],
        "extra": [],
        "fanout": ["--trigger", "ws"],
    },
    "workerman": {"profile": "workerman", "services": ["workerman"], "extra": [], "fanout": ["--trigger", "ws"]},
    "swoole": {"profile": "swoole", "services": ["swoole"], "extra": [], "fanout": ["--trigger", "ws"]},
    "reverb": {
        "profile": "reverb",
        "services": ["reverb"],
        "echo": False,  # the Pusher protocol has no "send me back what I sent": echo is not measured for Reverb
        # Pusher protocol: connect to /app/<key>, subscribe to a channel, broadcast with a signed REST call.
        "extra": ["--path", "/app/bench-key"],
        "fanout": [
            "--subscribe", "pusher", "--trigger", "pusher", "--channel", "bench",
            "--pusher-key", "bench-key", "--pusher-secret", "bench-secret", "--pusher-app-id", "bench",
            "--trigger-url", "http://target:8080/apps/bench/events",
        ],
    },
}

ECHO = [(64, 64), (512, 64)]            # (connections, message bytes)
IDLE = [1000, 10000]                    # connections held open
FANOUT = [(1000, 128), (5000, 128)]     # (subscribers, message bytes)


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


def compose(profile: str, *args: str, **kw) -> subprocess.CompletedProcess:
    return run(COMPOSE + ["--profile", profile, *args], **kw)


def container_ids(profile: str, services: list[str]) -> list[str]:
    ids = []
    for service in services:
        out = compose(profile, "ps", "-q", service).stdout.split()
        ids.extend(out)
    return ids


def mem_bytes(ids: list[str]) -> int:
    """Memory in use by the containers, summed (docker stats: usage without the page cache)."""
    if not ids:
        return 0
    out = run(["docker", "stats", "--no-stream", "--format", "{{.MemUsage}}", *ids]).stdout
    units = {"B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}
    total = 0
    for line in out.splitlines():
        m = re.match(r"\s*([\d.]+)\s*([KMG]?i?B)", line)
        if m:
            total += int(float(m.group(1)) * units[m.group(2)])
    return total


def loadgen(profile: str, mode: str, args: list[str]) -> dict:
    """One wsbench run, as the JSON object it prints."""
    cmd = COMPOSE + ["--profile", profile, "--profile", "loadgen", "run", "--rm", "-T", "loadgen", mode,
                     "--host", "target", "--port", "8080", *args]
    done = run(cmd)
    try:
        result = json.loads(done.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"error": (done.stderr or done.stdout).strip()[-500:] or "no output from wsbench"}
    if done.returncode != 0:
        result["exit_code"] = done.returncode  # wsbench found failures (lost connections, undelivered messages)
    return result


def wait_ready(profile: str, server: dict, timeout: float = 120.0) -> None:
    """Ready = one WebSocket connection can be opened and held (works for every contender, echo or not)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = loadgen(profile, "idle", ["--connections", "1", "--seconds", "1", *server["extra"]])
        if "error" not in r and r.get("established") == 1 and "exit_code" not in r:
            return
        time.sleep(2)
    raise RuntimeError(f"{profile}: not answering after {timeout:.0f} s")


def versions(profile: str, name: str) -> str:
    """Best effort: what was benchmarked, for the record."""
    def exec_(service: str, shell: str) -> str:
        return compose(profile, "exec", "-T", service, "sh", "-c", shell).stdout.strip()

    try:
        if name == "wsrv":
            # What is measured is the wsrvd image, so its own label says which commit it was built
            # from (docker build --build-arg REVISION=<sha>); the checkout this script sits in may
            # be a different commit (a branch that only adds the benchmark).
            image = os.environ.get("WSRVD_IMAGE", "wsrvd:local")
            revision = run(["docker", "image", "inspect", image, "--format",
                            '{{index .Config.Labels "org.opencontainers.image.revision"}}']).stdout.strip() or "unknown"
            php = exec_("worker", "php -r 'echo PHP_VERSION;'")
            return f"wsrvd image {image} built from commit {revision}, PHP {php} in the worker"
        if name == "workerman":
            return exec_("workerman", "php -r 'require \"vendor/autoload.php\"; echo \"Workerman \".Workerman\\Worker::VERSION.\", PHP \".PHP_VERSION;'")
        if name == "swoole":
            return exec_("swoole", "php -r 'echo \"Swoole \".SWOOLE_VERSION.\", PHP \".PHP_VERSION;'")
        if name == "reverb":
            laravel = exec_("reverb", "php artisan --version")
            reverb = exec_("reverb", "composer show laravel/reverb 2>/dev/null | grep -m1 '^versions'").split("*")[-1].strip()
            php = exec_("reverb", "php -r 'echo PHP_VERSION;'")
            return f"Laravel Reverb {reverb} ({laravel}), PHP {php}"
    except Exception as e:  # noqa: BLE001 - a record, not a gate
        return f"unknown ({e})"
    return "unknown"


def median_of(runs: list[dict], key: str):
    values = [r[key] for r in runs if key in r]
    return statistics.median(values) if values else None


def scenario_echo(profile: str, server: dict, repeats: int, seconds: int) -> list[dict]:
    rows = []
    for connections, size in ECHO:
        runs = [loadgen(profile, "echo", ["--connections", str(connections), "--threads", "4", "--size", str(size),
                                          "--seconds", str(seconds), "--warmup", "3", *server["extra"]]) for _ in range(repeats)]
        rows.append({"connections": connections, "size": size, "runs": runs})
        print(f"  echo {connections} conn: " + ", ".join(f"{r.get('msgs_per_sec', r.get('error'))}" for r in runs), flush=True)
    return rows


def scenario_idle(profile: str, server: dict, ids: list[str], hold: int) -> list[dict]:
    rows = []
    for connections in IDLE:
        time.sleep(3)
        baseline = mem_bytes(ids)
        samples: list[int] = []
        stop = threading.Event()

        def sample() -> None:
            while not stop.is_set():
                samples.append(mem_bytes(ids))
                stop.wait(1.5)

        thread = threading.Thread(target=sample)
        thread.start()
        result = loadgen(profile, "idle", ["--connections", str(connections), "--seconds", str(hold), "--threads", "4", *server["extra"]])
        stop.set()
        thread.join()
        peak = max(samples) if samples else 0
        established = result.get("established", 0)
        per = (peak - baseline) / established if established else None
        rows.append({"connections": connections, "result": result, "baseline_bytes": baseline, "peak_bytes": peak,
                     "bytes_per_connection": per})
        print(f"  idle {connections}: established {established}, baseline {baseline/1e6:.0f} MB, peak {peak/1e6:.0f} MB"
              + (f", {per/1024:.1f} KiB per connection" if per else ""), flush=True)
    return rows


def scenario_fanout(profile: str, server: dict, repeats: int, rounds: int) -> list[dict]:
    rows = []
    for connections, size in FANOUT:
        runs = [loadgen(profile, "fanout", ["--connections", str(connections), "--threads", "4", "--size", str(size),
                                            "--rounds", str(rounds), *server["extra"], *server["fanout"]]) for _ in range(repeats)]
        rows.append({"connections": connections, "size": size, "runs": runs})
        print(f"  fanout {connections} subs: " + ", ".join(f"{r.get('last_p50_us', r.get('error'))}" for r in runs), flush=True)
    return rows


def measure(name: str, args: argparse.Namespace) -> dict:
    server = SERVERS[name]
    profile = server["profile"]
    print(f"== {name}", flush=True)
    up = compose(profile, "up", "-d", "--build")
    if up.returncode != 0:
        return {"error": up.stderr.strip()[-800:]}
    try:
        wait_ready(profile, server)
        ids = container_ids(profile, server["services"])
        out: dict = {"versions": versions(profile, name)}
        if "echo" in args.scenarios and server.get("echo", True):
            out["echo"] = scenario_echo(profile, server, args.repeats, args.seconds)
        if "idle" in args.scenarios:
            out["idle"] = scenario_idle(profile, server, ids, args.hold)
        if "fanout" in args.scenarios:
            out["fanout"] = scenario_fanout(profile, server, args.repeats, args.rounds)
        return out
    except RuntimeError as e:  # one contender failing must not lose the others' numbers
        return {"error": str(e)}
    finally:
        compose(profile, "down", "-v", "--remove-orphans")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--servers", default="wsrv,workerman,swoole,reverb")
    ap.add_argument("--scenarios", default="echo,idle,fanout")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=15, help="echo: measured seconds per run")
    ap.add_argument("--hold", type=int, default=15, help="idle: seconds the connections are held")
    ap.add_argument("--rounds", type=int, default=30, help="fanout: broadcasts per run")
    ap.add_argument("--label", default=datetime.date.today().isoformat())
    args = ap.parse_args()
    args.scenarios = args.scenarios.split(",")

    info = run(["docker", "info", "--format", "{{.NCPU}} CPUs, {{.MemTotal}} bytes, {{.OperatingSystem}}, kernel {{.KernelVersion}}"]).stdout.strip()
    result = {"label": args.label, "date": datetime.datetime.now().isoformat(timespec="seconds"), "docker": info,
              "settings": {"echo": ECHO, "idle": IDLE, "fanout": FANOUT, "repeats": args.repeats, "seconds": args.seconds,
                           "hold": args.hold, "rounds": args.rounds},
              "servers": {}}
    for name in args.servers.split(","):
        result["servers"][name] = measure(name, args)
    out_dir = HERE / "results"
    out_dir.mkdir(exist_ok=True)
    path = out_dir / f"{args.label}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {path}")


if __name__ == "__main__":
    sys.exit(main())
