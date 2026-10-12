#!/usr/bin/env python3
"""Collect intervention-outcome episodes for the CPU world model.

Each *run* is one disposable, resource-limited container executing one
workload family (`osie_wm_lab serve`). A run consists of episodes:

    config (randomised) -> warmup -> PRE windows -> intervention -> settle -> POST windows

Interventions are randomised per episode: none (control), CPU quota (applied
from outside with `docker update --cpus`), cpuset size (`docker update
--cpuset-cpus`) or offered load (lab command). The container cannot change
its own limits. Requested and applied values are both recorded.

Usage:
    python3 ml/world_model/collect.py --out ml/world_model/data/v1 --runs-per-family 8
"""
import argparse
import json
import os
import platform
import queue
import random
import subprocess
import threading
import time
from datetime import datetime, timezone

IMAGE = "osie-wm-lab"
FAMILIES = {  # (CPU ms, blocking ms) per request; must match kFamilies in tools/world_model_lab.cpp
    "arith": (2.0, 0.0), "hash": (2.0, 0.0), "short": (0.3, 0.0), "burst": (1.0, 0.0), "mixed": (1.0, 2.0),
}
QUOTAS = [0.5, 1.0, 1.5, 2.0, 2.5, 3.0]
LOAD_FACTORS = [0.5, 0.75, 1.5, 2.0]
ACTIONS = [("none", 0.15), ("cpu_quota", 0.55), ("cpuset", 0.15), ("offered_load", 0.15)]
LANES = ["0-3", "4-7"]  # disjoint CPUs of the Docker VM, one experiment per lane at a time
WINDOW_MS = 500


def sh(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True).stdout.strip()


def cpus_of(lane, k):
    first = int(lane.split("-")[0])
    return f"{first}-{first + k - 1}" if k > 1 else str(first)


def environment():
    info = json.loads(sh("docker", "info", "--format", "{{json .}}"))
    return {
        "collected_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "kernel": info.get("KernelVersion"),
        "os": info.get("OperatingSystem"),
        "architecture": info.get("Architecture"),
        "vm_cpus": info.get("NCPU"),
        "vm_memory_bytes": info.get("MemTotal"),
        "cgroup_version": info.get("CgroupVersion"),
        "cgroup_driver": info.get("CgroupDriver"),
        "container_runtime": f"docker {info.get('ServerVersion')}",
        "hypervisor": "Docker Desktop VM (Apple Virtualization framework)" if platform.system() == "Darwin" else "unknown",
        "host_machine": sh("sysctl", "-n", "machdep.cpu.brand_string", check=False) if platform.system() == "Darwin" else platform.processor(),
        "host_cpus": os.cpu_count(),
        "cpu_frequency": "not exposed inside the VM",
        "image": sh("docker", "image", "inspect", IMAGE, "--format", "{{.Id}}"),
    }


def host_load():
    if platform.system() == "Darwin":
        return sh("sysctl", "-n", "vm.loadavg", check=False)
    return open("/proc/loadavg").read().strip()


class Lab:
    """One running lab container, driven over stdin/stdout."""

    def __init__(self, name, lane, mem="512m"):
        self.name = name
        self.proc = subprocess.Popen(
            ["docker", "run", "-i", "--rm", "--name", name, "--cpus", "4", "--cpuset-cpus", lane,
             "--memory", mem, "--pids-limit", "128", "--network", "none", "--read-only",
             "--cap-drop", "ALL", "--security-opt", "no-new-privileges", IMAGE],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def cmd(self, line, timeout=60):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        if not out:
            raise RuntimeError(f"{self.name}: lab exited during '{line}'")
        return json.loads(out)

    def update(self, **kw):
        args = ["docker", "update"]
        for k, v in kw.items():
            args += [f"--{k.replace('_', '-')}", str(v)]
        t0 = time.time()
        sh(*args, self.name)
        return time.time() - t0

    def close(self):
        try:
            self.cmd("QUIT")
        except Exception:
            pass
        subprocess.run(["docker", "kill", self.name], capture_output=True)
        self.proc.wait(timeout=30)


CF_QUOTAS = [0.5, 1.0, 1.5, 2.0]


def run_counterfactual(spec, out_path, lock):
    """Section 13: the same workload configuration under each candidate quota
    (randomised order), so models can be scored on ranking interventions."""
    rng = random.Random(spec["seed"])
    lane, family = spec["lane"], spec["family"]
    lab = Lab(f"osie-wm-{spec['run_id']}", lane)
    try:
        calib = lab.cmd("CALIBRATE")
        for cfg_i in range(spec["configs"]):
            workers = rng.choice([2, 3, 4])
            q_before = 1.0
            cpu_ms, block_ms = FAMILIES[family]
            util = rng.uniform(0.6, 1.3)
            rate = max(5.0, util * min(q_before, workers * cpu_ms / (cpu_ms + block_ms)) / (cpu_ms / 1000.0))
            seed = rng.randrange(1 << 30)
            order = CF_QUOTAS[:]
            rng.shuffle(order)
            for q_after in order:
                lab.update(cpus=q_before, cpuset_cpus=lane)
                lab.cmd(f"CONFIG family={family} rate={rate:.3f} workers={workers} seed={seed}")
                warm = lab.cmd(f"WINDOWS n=4 ms={WINDOW_MS} tag=warmup")
                pre = lab.cmd(f"WINDOWS n=6 ms={WINDOW_MS} tag=pre")
                kind = "none" if q_after == q_before else "cpu_quota"
                action = {"kind": kind, "previous": q_before if kind != "none" else 0.0,
                          "proposed": q_after if kind != "none" else 0.0, "requested_at": time.time()}
                if kind == "cpu_quota":
                    action["apply_latency_s"] = lab.update(cpus=q_after)
                settle = lab.cmd(f"WINDOWS n=2 ms={WINDOW_MS} tag=settle")
                post = lab.cmd(f"WINDOWS n=6 ms={WINDOW_MS} tag=post")
                action["applied"] = post["windows"][-1].get("quota_cores")
                valid = all("usage_cores" in w for w in pre["windows"] + post["windows"]) and \
                    action["applied"] is not None and abs(action["applied"] - q_after) <= 0.02
                rec = {"run_id": spec["run_id"], "episode": len(order) * cfg_i + order.index(q_after),
                       "counterfactual_group": f"{spec['run_id']}/cfg{cfg_i}", "candidate_quota": q_after,
                       "family": family, "lane": lane, "contention": False, "seed": spec["seed"],
                       "config": {"workers": workers, "quota_before": q_before, "offered_rate": rate, "target_util": util},
                       "action": action, "valid": valid, "calibration": calib,
                       "warmup": warm["windows"], "pre": pre["windows"], "settle": settle["windows"], "post": post["windows"]}
                with lock:
                    with open(out_path, "a") as f:
                        f.write(json.dumps(rec) + "\n")
    finally:
        lab.close()


def run_batch(spec, out_path, lock):
    """Held-out workload *class*: closed-loop batch threads (always runnable),
    started with `docker exec` inside the lab container so they share its
    cgroup. The request server runs at a negligible 5 req/s."""
    rng = random.Random(spec["seed"])
    lane = spec["lane"]
    name = f"osie-wm-{spec['run_id']}"
    lab = Lab(name, lane)
    try:
        calib = lab.cmd("CALIBRATE")
        for ep in range(spec["episodes"]):
            threads = rng.choice([1, 2, 3, 4])
            q_before = rng.choice(QUOTAS)
            q_after = rng.choice([q for q in QUOTAS if q != q_before])
            lab.update(cpus=q_before, cpuset_cpus=lane)
            lab.cmd(f"CONFIG family=short rate=5 workers=1 seed={rng.randrange(1 << 30)}")
            load = subprocess.Popen(["docker", "exec", name, "/usr/local/bin/osie_cpu_lab", "load", "--mode", "spin",
                                     "--threads", str(threads), "--duration", "11"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                warm = lab.cmd(f"WINDOWS n=3 ms={WINDOW_MS} tag=warmup")
                pre = lab.cmd(f"WINDOWS n=6 ms={WINDOW_MS} tag=pre")
                action = {"kind": "cpu_quota", "previous": q_before, "proposed": q_after, "requested_at": time.time(),
                          "apply_latency_s": lab.update(cpus=q_after)}
                settle = lab.cmd(f"WINDOWS n=2 ms={WINDOW_MS} tag=settle")
                post = lab.cmd(f"WINDOWS n=6 ms={WINDOW_MS} tag=post")
            finally:
                load.wait(timeout=30)
            action["applied"] = post["windows"][-1].get("quota_cores")
            valid = action["applied"] is not None and abs(action["applied"] - q_after) <= 0.02
            rec = {"run_id": spec["run_id"], "episode": ep, "family": "batch_spin", "lane": lane, "contention": False,
                   "seed": spec["seed"], "config": {"workers": threads, "quota_before": q_before, "batch_threads": threads},
                   "action": action, "valid": valid, "calibration": calib,
                   "warmup": warm["windows"], "pre": pre["windows"], "settle": settle["windows"], "post": post["windows"]}
            with lock:
                with open(out_path, "a") as f:
                    f.write(json.dumps(rec) + "\n")
    finally:
        lab.close()


def run_one(spec, out_path, lock):
    rng = random.Random(spec["seed"])
    lane, family = spec["lane"], spec["family"]
    run_id = spec["run_id"]
    neighbour = None
    if spec["contention"]:
        neighbour = f"osie-wm-nb-{run_id}"
        sh("docker", "run", "-d", "--rm", "--name", neighbour, "--cpus", "1", "--cpuset-cpus", lane,
           "--memory", "128m", "--pids-limit", "32", "--network", "none", "--read-only", "--cap-drop", "ALL",
           "--entrypoint", "/usr/local/bin/osie_cpu_lab", IMAGE, "load", "--mode", "spin", "--threads", "2",
           "--duration", "3600")
    lab = Lab(f"osie-wm-{run_id}", lane)
    try:
        calib = lab.cmd("CALIBRATE")
        load_start = host_load()
        for ep in range(spec["episodes"]):
            workers = rng.choice([1, 2, 3, 4])
            q_before = rng.choice(QUOTAS)
            util = rng.uniform(0.3, 1.3)
            cpu_ms, block_ms = FAMILIES[family]
            # Bottleneck capacity in cores: the quota, or the workers' CPU share
            # when requests also block (each worker is busy cpu+block ms per request).
            cap = min(q_before, workers * cpu_ms / (cpu_ms + block_ms))
            rate = max(5.0, util * cap / (cpu_ms / 1000.0))
            lab.update(cpus=q_before, cpuset_cpus=lane)
            r = lab.cmd(f"CONFIG family={family} rate={rate:.3f} workers={workers} seed={rng.randrange(1 << 30)}")
            if r.get("type") != "ok":
                raise RuntimeError(f"config failed: {r}")
            warm = lab.cmd(f"WINDOWS n=4 ms={WINDOW_MS} tag=warmup")
            pre = lab.cmd(f"WINDOWS n=6 ms={WINDOW_MS} tag=pre")

            kinds, weights = zip(*ACTIONS)
            kind = rng.choices(kinds, weights)[0]
            action = {"kind": kind, "previous": 0.0, "proposed": 0.0, "requested_at": time.time()}
            if kind == "cpu_quota":
                action["previous"] = q_before
                action["proposed"] = rng.choice([q for q in QUOTAS if q != q_before])
                action["apply_latency_s"] = lab.update(cpus=action["proposed"])
            elif kind == "cpuset":
                k = rng.choice([1, 2, 3])
                action["previous"], action["proposed"] = 4.0, float(k)
                action["apply_latency_s"] = lab.update(cpuset_cpus=cpus_of(lane, k))
            elif kind == "offered_load":
                f = rng.choice(LOAD_FACTORS)
                action["previous"], action["proposed"] = 1.0, f
                lab.cmd(f"RATE factor={f}")
            settle = lab.cmd(f"WINDOWS n=2 ms={WINDOW_MS} tag=settle")
            post = lab.cmd(f"WINDOWS n=6 ms={WINDOW_MS} tag=post")

            # Applied value as the kernel reports it in the post windows.
            pw = post["windows"]
            applied = None
            if kind == "cpu_quota":
                applied = pw[-1].get("quota_cores")
            elif kind == "cpuset":
                applied = pw[-1].get("cpuset_cpus")
            elif kind == "offered_load":
                applied = post["offered_rate"] / pre["offered_rate"] if pre["offered_rate"] else None
            action["applied"] = applied
            valid = all("usage_cores" in w for w in pre["windows"] + pw)
            if kind in ("cpu_quota", "cpuset") and (applied is None or abs(applied - action["proposed"]) > 0.02):
                valid = False
            rec = {
                "run_id": run_id, "episode": ep, "family": family, "lane": lane, "contention": spec["contention"],
                "seed": spec["seed"], "config": {"workers": workers, "quota_before": q_before, "offered_rate": rate,
                                                 "target_util": util},
                "action": action, "valid": valid, "calibration": calib, "host_load_at_run_start": load_start,
                "warmup": warm["windows"], "pre": pre["windows"], "settle": settle["windows"], "post": pw,
            }
            with lock:
                with open(out_path, "a") as f:
                    f.write(json.dumps(rec) + "\n")
    finally:
        lab.close()
        if neighbour:
            subprocess.run(["docker", "kill", neighbour], capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--runs-per-family", type=int, default=8)
    ap.add_argument("--episodes", type=int, default=12)
    ap.add_argument("--families", default=",".join(FAMILIES))
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--lanes", type=int, default=2)
    ap.add_argument("--counterfactual", action="store_true", help="Section 13 design: each config under every candidate quota")
    ap.add_argument("--configs", type=int, default=3, help="configs per counterfactual run")
    ap.add_argument("--batch", action="store_true", help="held-out workload class: closed-loop batch threads")
    ap.add_argument("--run-offset", type=int, default=100, help="run-id offset so counterfactual runs never collide")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "environment.json"), "w") as f:
        json.dump(environment(), f, indent=1)

    rng = random.Random(args.seed)
    specs = []
    for fam in args.families.split(","):
        for i in range(args.runs_per_family):
            if args.batch:
                specs.append({"family": "batch_spin", "run_id": f"batch-{args.run_offset + i:03d}",
                              "seed": rng.randrange(1 << 30), "contention": False, "episodes": args.episodes})
            elif args.counterfactual:
                specs.append({"family": fam, "run_id": f"cf-{fam}-{args.run_offset + i:03d}", "seed": rng.randrange(1 << 30),
                              "contention": False, "configs": args.configs})
            else:
                specs.append({"family": fam, "run_id": f"{fam}-{i:02d}", "seed": rng.randrange(1 << 30),
                              "contention": i % 2 == 1, "episodes": args.episodes})
    rng.shuffle(specs)  # interleave families over time so drift is not confounded with family
    q = queue.Queue()
    for s in specs:
        q.put(s)
    lock = threading.Lock()
    out_path = os.path.join(args.out, "episodes.jsonl")
    errors = []

    def lane_worker(lane):
        while True:
            try:
                s = q.get_nowait()
            except queue.Empty:
                return
            s = dict(s, lane=lane)
            t0 = time.time()
            try:
                (run_batch if args.batch else run_counterfactual if args.counterfactual else run_one)(s, out_path, lock)
                print(f"[{lane}] {s['run_id']} done in {time.time() - t0:.0f}s ({q.qsize()} left)", flush=True)
            except Exception as e:  # keep going; the run is recorded as failed
                errors.append((s["run_id"], str(e)))
                print(f"[{lane}] {s['run_id']} FAILED: {e}", flush=True)

    threads = [threading.Thread(target=lane_worker, args=(LANES[i],)) for i in range(args.lanes)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    with open(os.path.join(args.out, "collection_log.json"), "w") as f:
        json.dump({"runs_planned": len(specs), "failed": errors}, f, indent=1)
    print(f"done: {len(specs) - len(errors)} runs ok, {len(errors)} failed")


if __name__ == "__main__":
    main()
