#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Load and churn runs of the Pusher gateway against a real wsrvd (ADR-0042 A2).

It starts a daemon, the gateway worker (worker.php: the code `php artisan wsrv:echo` runs) and a fleet of
Pusher-protocol clients (load.mjs), pins the three to different cores so that the numbers are the gateway's and
not the load generator's, samples CPU and memory of the daemon and the worker once a second, reads the
gateway's own statistics line, and checks what an operator would check: after the last client has left, every
gauge of the gateway is back to zero.

    python3 run.py --daemon ../../../../build/release/apps/wsrvd/wsrvd --scenario join --clients 2000
    python3 run.py --daemon ... --scenario whisper --members 5 --groups 100 --rate 10 --duration 30
    python3 run.py --daemon ... --scenario churn --concurrency 100 --duration 600 --procs 4 --kill-every 20

Results go to results/<label>.json (not committed). Exit code 0 only if every check holds.
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TICKS = os.sysconf("SC_CLK_TCK")
KEY = "load-key-0123456789"
SECRET = "load-secret-0123456789abcdef0123456789"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--daemon", required=True, help="path of the wsrvd binary (a release build for numbers)")
    p.add_argument("--scenario", choices=["join", "whisper", "churn"], required=True)
    p.add_argument("--label", default=None)
    p.add_argument("--procs", type=int, default=2, help="load generator processes")
    p.add_argument("--cores-daemon", default="auto", help="taskset list for wsrvd; 'auto' = the first two cores of a machine with 8 or more, '' = no pinning")
    p.add_argument("--cores-worker", default="auto", help="taskset list for the PHP worker; 'auto' = the third core")
    p.add_argument("--cores-clients", default="auto", help="taskset list for the load generators; 'auto' = the next 8 cores")
    p.add_argument("--threads", type=int, default=2, help="wsrvd event loops")
    p.add_argument("--stats-interval", type=float, default=2.0)
    p.add_argument("--options", default="{}", help="JSON of GatewayConfig arguments for the worker")
    p.add_argument("--broker-topics", type=int, default=500000)
    p.add_argument("--broker-subscriptions", type=int, default=1000000)
    p.add_argument("--max-connections", type=int, default=60000)
    p.add_argument("--kill-every", type=float, default=0, help="churn: SIGKILL one load process this often (s)")
    p.add_argument("--settle", type=float, default=60, help="seconds to wait for the gauges to return to zero")
    # scenario arguments, passed on (totals are split over --procs)
    p.add_argument("--clients", type=int, default=1000)
    p.add_argument("--public", type=int, default=1)
    p.add_argument("--private", type=int, default=1)
    p.add_argument("--presence", type=int, default=1)
    p.add_argument("--ramp", type=float, default=0, help="connections per second, 0 = all at once")
    p.add_argument("--hold", type=float, default=5)
    p.add_argument("--room-size", type=int, default=50)
    p.add_argument("--members", type=int, default=5)
    p.add_argument("--groups", type=int, default=20)
    p.add_argument("--rate", type=float, default=5)
    p.add_argument("--duration", type=float, default=30)
    p.add_argument("--concurrency", type=int, default=50)
    p.add_argument("--abuse", type=float, default=0.02)
    return p.parse_args()


def auto_cores(a):
    """Where each part runs when --cores-* say 'auto': separate cores on a machine that has them, otherwise
    nowhere in particular (and the numbers say so)."""
    count = os.cpu_count() or 1
    layout = {"daemon": "0-1", "worker": "2", "clients": f"3-{min(count, 11) - 1}"} if count >= 8 else None
    if layout is None:
        sys.stderr.write(f"note: {count} CPUs: nothing is pinned, so the load generator competes with what it measures\n")
    for name in ("daemon", "worker", "clients"):
        if getattr(a, f"cores_{name}") == "auto":
            setattr(a, f"cores_{name}", layout[name] if layout else "")


def pinned(cores, argv):
    return ["taskset", "-c", cores] + argv if cores and shutil.which("taskset") else argv


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def cpu_seconds(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / TICKS
    except (OSError, IndexError):
        return None


def rss_kib(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def parse_stats_line(line):
    m = re.search(r"gateway (?:final )?((?:[a-z_]+=\d+ ?)+)\s*$", line)
    if not m:
        return None
    return {k: int(v) for k, v in (kv.split("=") for kv in m.group(1).split())}


class Run:
    def __init__(self, a):
        self.a = a
        self.dir = tempfile.mkdtemp(prefix="wsrv-gwload-")
        self.samples = []  # (t, daemon_cpu_s, worker_cpu_s, daemon_rss_kib, worker_rss_kib)
        self.stats = []  # (t, dict)
        self.stop = threading.Event()
        self.daemon = self.worker = None
        self.worker_log = os.path.join(self.dir, "worker.log")
        self.daemon_log = os.path.join(self.dir, "daemon.log")

    def start_daemon(self):
        a = self.a
        self.port = free_port()
        env = dict(os.environ)
        env.update({
            "WSRVD_ALLOW_MISSING_ORIGIN": "true",
            "WSRVD_MAX_CONNECTIONS_PER_IP": "0",
            "WSRVD_MAX_NEW_CONNECTIONS_PER_IP_PER_SECOND": "0",
            "WSRVD_BROKER_MAX_TOPICS": str(a.broker_topics),
            "WSRVD_BROKER_MAX_SUBSCRIPTIONS": str(a.broker_subscriptions),
            "WSRVD_MAX_CONNECTIONS": str(a.max_connections),
        })
        argv = [a.daemon, f"--ipc-socket={self.dir}/ipc.sock", f"--listen-port={self.port}", f"--threads={a.threads}",
                "--broker=true", "--allow-origin=https://app.example.com", "--shutdown-grace-ms=500",
                "--log-level=warn"]
        log = open(self.daemon_log, "w")
        self.daemon = subprocess.Popen(pinned(a.cores_daemon, argv), env=env, stdout=log, stderr=log)
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.daemon.poll() is not None:
                sys.exit("wsrvd did not start:\n" + open(self.daemon_log).read())
            if os.path.exists(f"{self.dir}/ipc.sock"):
                try:
                    with socket.create_connection(("127.0.0.1", self.port), timeout=1):
                        return
                except OSError:
                    pass
            time.sleep(0.1)
        sys.exit("wsrvd did not listen:\n" + open(self.daemon_log).read())

    def start_worker(self):
        a = self.a
        argv = ["php", os.path.join(HERE, "worker.php"), f"--socket={self.dir}/ipc.sock", f"--key={KEY}",
                f"--secret={SECRET}", f"--stats-interval={a.stats_interval}", f"--options={a.options}"]
        self.worker = subprocess.Popen(pinned(a.cores_worker, argv), stdout=subprocess.DEVNULL,
                                       stderr=open(self.worker_log, "w"))
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.worker.poll() is not None:
                sys.exit("worker died:\n" + open(self.worker_log).read())
            if "connected to wsrvd" in open(self.worker_log).read():
                return
            time.sleep(0.1)
        sys.exit("worker did not connect:\n" + open(self.worker_log).read())

    def sampler(self):
        position = 0
        t0 = time.time()
        while not self.stop.is_set():
            now = time.time() - t0
            self.samples.append((now, cpu_seconds(self.daemon.pid), cpu_seconds(self.worker.pid),
                                 rss_kib(self.daemon.pid), rss_kib(self.worker.pid)))
            with open(self.worker_log) as f:
                f.seek(position)
                chunk = f.read()
                position = f.tell()
            for line in chunk.splitlines():
                s = parse_stats_line(line)
                if s:
                    self.stats.append((now, s))
            self.stop.wait(1.0)

    def load_argv(self, index, procs, offset_base):
        a = self.a
        common = [f"--url=ws://127.0.0.1:{self.port}", f"--key={KEY}", f"--secret={SECRET}", f"--scenario={a.scenario}"]
        if a.scenario == "join":
            share = a.clients // procs + (1 if index < a.clients % procs else 0)
            start = sum(a.clients // procs + (1 if i < a.clients % procs else 0) for i in range(index))
            return common + [f"--clients={share}", f"--offset={offset_base + start}", f"--public={a.public}",
                             f"--private={a.private}", f"--presence={a.presence}", f"--ramp={a.ramp / procs}",
                             f"--hold={a.hold}", f"--room-size={a.room_size}"]
        if a.scenario == "whisper":
            groups = a.groups // procs + (1 if index < a.groups % procs else 0)
            start = sum(a.groups // procs + (1 if i < a.groups % procs else 0) for i in range(index))
            return common + [f"--members={a.members}", f"--groups={groups}", f"--offset={offset_base + start}",
                             f"--rate={a.rate}", f"--duration={a.duration}"]
        share = max(1, a.concurrency // procs)
        return common + [f"--concurrency={share}", f"--duration={a.duration}", f"--abuse={a.abuse}",
                         f"--offset={offset_base + index * 1000}", f"--hold={a.hold if a.hold != 5 else 1.5}"]

    def spawn_load(self, index, procs, offset_base=0, duration_override=None):
        argv = ["node", os.path.join(HERE, "load.mjs")] + self.load_argv(index, procs, offset_base)
        if duration_override is not None:
            argv = [x if not x.startswith("--duration=") else f"--duration={duration_override}" for x in argv]
        return subprocess.Popen(pinned(self.a.cores_clients, argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def run_clients(self):
        a = self.a
        procs = [self.spawn_load(i, a.procs) for i in range(a.procs)]
        results, kills = [], 0
        if a.scenario == "churn" and a.kill_every > 0:
            end = time.time() + a.duration
            generation = 1
            while time.time() < end:
                time.sleep(min(a.kill_every, max(0.0, end - time.time())))
                if time.time() >= end:
                    break
                victim = procs.pop(0)
                victim.kill()
                victim.communicate()
                kills += 1
                procs.append(self.spawn_load(generation % a.procs, a.procs, offset_base=generation * 100000,
                                             duration_override=max(5.0, end - time.time())))
                generation += 1
        limit = 180 + a.duration + a.hold + (a.clients / a.ramp if a.ramp else 0)
        for p in procs:
            try:
                out, err = p.communicate(timeout=limit)
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                results.append({"error": f"load process still running after {limit:.0f} s: killed"})
                continue
            if err.strip():
                sys.stderr.write(err)
            line = out.strip().splitlines()[-1] if out.strip() else ""
            if p.returncode == 0 and line:
                results.append(json.loads(line))
            else:
                results.append({"error": f"load process exited {p.returncode}", "stdout": out[-300:]})
        return results, kills

    def settle(self):
        deadline = time.time() + self.a.settle
        zero = ("connections", "channels", "joins_in_progress", "presence_channels", "presence_members")
        while time.time() < deadline:
            if self.stats and all(self.stats[-1][1].get(k) == 0 for k in zero):
                return True
            time.sleep(0.5)
        return False

    def finish(self):
        self.stop.set()
        for proc in (self.worker, self.daemon):
            if proc and proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in (self.worker, self.daemon):
            if proc:
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()


def summarize(run, results, kills, settled, started, ended):
    s = run.samples
    window = [x for x in s if started <= x[0] <= ended and x[2] is not None]

    def cpu_pct(index):
        pts = [x for x in s if x[index] is not None]
        out = []
        for earlier, later in zip(pts, pts[1:]):
            dt = later[0] - earlier[0]
            if dt > 0 and started <= later[0] <= ended:
                out.append(100.0 * (later[index] - earlier[index]) / dt)
        return out

    worker_cpu, daemon_cpu = cpu_pct(2), cpu_pct(1)
    frames_rate = []
    for (t1, a1), (t2, a2) in zip(run.stats, run.stats[1:]):
        if t2 - t1 > 0 and started <= t2 <= ended:
            frames_rate.append((a2["frames"] - a1["frames"]) / (t2 - t1))
    joins_rate = []
    for (t1, a1), (t2, a2) in zip(run.stats, run.stats[1:]):
        if t2 - t1 > 0 and started <= t2 <= ended:
            joins_rate.append((a2["joins"] - a1["joins"]) / (t2 - t1))
    peak = max((x[1] for x in run.stats), key=lambda d: d["connections"], default={})
    first = run.stats[0][1] if run.stats else {}
    last = run.stats[-1][1] if run.stats else {}
    worker_rss = [x[4] for x in s if x[4]]
    baseline_rss = worker_rss[0] if worker_rss else 0
    peak_rss = max(worker_rss) if worker_rss else 0
    per_conn = None
    if peak.get("connections"):
        per_conn = round((peak_rss - baseline_rss) * 1024 / peak["connections"])
    return {
        "scenario": run.a.scenario,
        "args": {k: v for k, v in vars(run.a).items() if k not in ("daemon",)},
        "elapsed_s": round(ended - started, 1),
        "settled_to_zero": settled,
        "load_kills": kills,
        "clients": results,
        "gateway": {
            "peak": peak,
            "last": last,
            "worker_cpu_pct_mean": round(sum(worker_cpu) / len(worker_cpu), 1) if worker_cpu else None,
            "worker_cpu_pct_max": round(max(worker_cpu), 1) if worker_cpu else None,
            "daemon_cpu_pct_mean": round(sum(daemon_cpu) / len(daemon_cpu), 1) if daemon_cpu else None,
            "daemon_cpu_pct_max": round(max(daemon_cpu), 1) if daemon_cpu else None,
            "frames_per_s_max": round(max(frames_rate), 1) if frames_rate else None,
            "joins_per_s_max": round(max(joins_rate), 1) if joins_rate else None,
            "worker_rss_kib_baseline": baseline_rss,
            "worker_rss_kib_peak": peak_rss,
            "worker_rss_kib_end": worker_rss[-1] if worker_rss else None,
            "worker_rss_bytes_per_peak_connection": per_conn,
            "php_memory_bytes_first_stats": first.get("memory_bytes"),
            "php_memory_bytes_peak_conns": peak.get("memory_bytes"),
            "php_memory_bytes_last": last.get("memory_bytes"),
        },
        "daemon_rss_kib_peak": max((x[3] for x in s if x[3]), default=None),
    }


def main():
    a = parse_args()
    auto_cores(a)
    label = a.label or f"{time.strftime('%Y%m%d-%H%M%S')}-{a.scenario}"
    run = Run(a)
    failures = []
    try:
        run.start_daemon()
        run.start_worker()
        sampler = threading.Thread(target=run.sampler, daemon=True)
        sampler.start()
        time.sleep(a.stats_interval + 1)  # a baseline reading
        started = run.samples[-1][0]
        results, kills = run.run_clients()
        ended = run.samples[-1][0]
        settled = run.settle()
        summary = summarize(run, results, kills, settled, started, ended)
        # checks
        if not settled:
            failures.append(f"the gateway did not return to zero within {a.settle} s: {run.stats[-1][1] if run.stats else None}")
        for r in results:
            if "error" in r:
                failures.append(f"a load process failed: {r}")
            elif r.get("counters", {}).get("subscribeTimeouts"):
                failures.append(f"{r['counters']['subscribeTimeouts']} subscribes were never answered")
        if a.scenario == "join":
            for r in results:
                if r.get("failures", {}).get("connect") or r.get("failures", {}).get("subscribe"):
                    failures.append(f"join failures: {r.get('failures')}")
                if r.get("stillOpenAfterHold", 0) != r.get("ready", 0):
                    failures.append(f"{r.get('ready', 0) - r.get('stillOpenAfterHold', 0)} clients lost during the hold")
        summary["failures"] = failures
    finally:
        run.finish()
        final = [parse_stats_line(l) for l in open(run.worker_log).read().splitlines() if "gateway final" in l]
        if final and final[-1]:
            pass
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    out = os.path.join(HERE, "results", f"{label}.json")
    with open(out, "w") as f:
        json.dump(summary, f, indent=2)
    shutil.copy(run.worker_log, os.path.join(HERE, "results", f"{label}.worker.log"))
    shutil.copy(run.daemon_log, os.path.join(HERE, "results", f"{label}.daemon.log"))
    print(json.dumps({k: summary[k] for k in ("scenario", "elapsed_s", "settled_to_zero", "gateway", "failures")}, indent=2))
    print(f"results: {out}")
    shutil.rmtree(run.dir, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
