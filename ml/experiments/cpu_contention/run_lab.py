# SPDX-License-Identifier: Apache-2.0
"""Milestone 5: can a tiny model diagnose Linux CPU bottlenecks from native
telemetry better than deterministic rules?

Every scenario runs in disposable Docker containers on the Docker VM, pinned
to 2 vCPUs (probe) with 256 MB memory limits, no network and ~8 s lifetime.
Nothing touches the host OS configuration; nothing runs privileged.

Scenario classes (the "cause" label) and generalisation split:
  none            probe alone                             | test: same
  busy_elsewhere  load on *other* CPUs (label: none)      | train spin x4, test hash x4
  contention      load pinned to the probe's CPUs         | train spin 2/4, test hash 3 + duty 0.7 x6
  throttling      probe's own cgroup exceeds cpu.max      | train quota .5/.3 + spin, test quota .8 + duty x2
  io              fsync-heavy writer on the probe's CPUs   | train 2 writers, test 4 writers
Test scenarios use workload types / quotas / thread counts never seen in training.

  python3 ml/experiments/cpu_contention/run_lab.py run     # collect (~5 min)
  python3 ml/experiments/cpu_contention/run_lab.py analyze # train/evaluate models
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DATA = os.path.join(HERE, "data")
IMAGE = "osie-linux-dev"
BIN = "/b/osie_cpu_lab"
PROBE_CPUS = "2,3"
OTHER_CPUS = "6,7,8,9"
LIMITS = ["--memory=256m", "--pids-limit=256", "--network=none"]
WORK = "1000000"
DURATION = "8"

# (run_id, label, split, background spec or None, probe extra docker args, in-container load or None)
SCENARIOS = [
    ("none_a", "none", "train", None, [], None),
    ("none_b", "none", "train", None, [], None),
    ("none_c", "none", "train", None, [], None),
    ("elsewhere_spin4_a", "none", "train", (OTHER_CPUS, "spin", 4, 1.0), [], None),
    ("elsewhere_spin4_b", "none", "train", (OTHER_CPUS, "spin", 4, 1.0), [], None),
    ("contention_spin2_a", "contention", "train", (PROBE_CPUS, "spin", 2, 1.0), [], None),
    ("contention_spin2_b", "contention", "train", (PROBE_CPUS, "spin", 2, 1.0), [], None),
    ("contention_spin4_a", "contention", "train", (PROBE_CPUS, "spin", 4, 1.0), [], None),
    ("contention_spin4_b", "contention", "train", (PROBE_CPUS, "spin", 4, 1.0), [], None),
    ("throttle_q50_a", "throttling", "train", None, ["--cpus=0.5"], ("spin", 1, 1.0)),
    ("throttle_q50_b", "throttling", "train", None, ["--cpus=0.5"], ("spin", 1, 1.0)),
    ("throttle_q30_a", "throttling", "train", None, ["--cpus=0.3"], ("spin", 1, 1.0)),
    ("io_w2_a", "io", "train", (PROBE_CPUS, "io", 2, 1.0), [], None),
    ("io_w2_b", "io", "train", (PROBE_CPUS, "io", 2, 1.0), [], None),
    # ---- held-out workloads / parameters
    ("none_t", "none", "test", None, [], None),
    ("elsewhere_hash4_t", "none", "test", (OTHER_CPUS, "hash", 4, 1.0), [], None),
    ("contention_hash3_t", "contention", "test", (PROBE_CPUS, "hash", 3, 1.0), [], None),
    ("contention_duty6_t", "contention", "test", (PROBE_CPUS, "duty", 6, 0.7), [], None),
    ("throttle_q80_duty2_t", "throttling", "test", None, ["--cpus=0.8"], ("duty", 2, 0.6)),
    ("io_w4_t", "io", "test", (PROBE_CPUS, "io", 4, 1.0), [], None),
]

FEATURES = ["host_busy", "host_user", "host_system", "host_iowait", "runnable", "blocked", "load1", "ctxt_per_s",
            "psi_cpu_some", "psi_mem_some", "psi_io_some", "psi_io_full", "cg_usage_cores", "cg_throttled_ratio",
            "cg_throttled_frac", "cg_quota_cores", "cg_psi_cpu_some", "cpuset"]
CLASSES = ["none", "contention", "throttling", "io"]


def docker(args, **kw):
    return subprocess.run(["docker", *args], check=True, capture_output=True, text=True, **kw)


def run():
    os.makedirs(DATA, exist_ok=True)
    binmount = ["-v", f"{ROOT}/build-linux:/b:ro"]
    t0 = time.time()
    for rid, label, split, bg, extra, inload in SCENARIOS:
        out = f"/out/{rid}.jsonl"
        bg_name = None
        if bg:
            cpus, mode, threads, duty = bg
            bg_name = f"osie_bg_{rid}"
            docker(["run", "-d", "--rm", "--name", bg_name, f"--cpuset-cpus={cpus}", *LIMITS, *binmount, IMAGE, BIN, "load",
                    "--mode", mode, "--threads", str(threads), "--duty", str(duty), "--duration", "14"])
            time.sleep(1.5)
        probe = f"{BIN} probe --duration {DURATION} --work {WORK} --label {label} --run {rid} --out {out}"
        if inload:
            mode, threads, duty = inload
            probe = f"{BIN} load --mode {mode} --threads {threads} --duty {duty} --duration 12 & exec {probe}"
        try:
            docker(["run", "--rm", f"--cpuset-cpus={PROBE_CPUS}", *LIMITS, *extra, *binmount, "-v", f"{DATA}:/out",
                    IMAGE, "sh", "-c", probe], timeout=60)
        finally:
            if bg_name:
                subprocess.run(["docker", "rm", "-f", bg_name], capture_output=True)
        with open(os.path.join(DATA, f"{rid}.split"), "w") as f:
            f.write(split)
        n = sum(1 for _ in open(os.path.join(DATA, f"{rid}.jsonl")))
        print(f"{rid:<24} {label:<11} {split:<5} windows={n}", flush=True)
    print(f"collection wall time {time.time() - t0:.0f}s")


def load_windows():
    rows = []
    for fn in sorted(os.listdir(DATA)):
        if not fn.endswith(".jsonl"):
            continue
        split = open(os.path.join(DATA, fn.replace(".jsonl", ".split"))).read().strip()
        for line in open(os.path.join(DATA, fn)):
            r = json.loads(line)
            r["split"] = split
            rows.append(r)
    return rows


def rules_a_priori(r) -> str:
    """Deterministic diagnosis from documented kernel semantics; thresholds fixed
    in advance (not fitted): throttling if >10% of periods throttled; CPU
    contention if this cgroup's tasks waited for CPU >5% of the time (cgroup
    PSI); I/O if tasks stalled on I/O >10% of the time; otherwise none."""
    if r.get("cg_throttled_ratio", 0) > 0.10:
        return "throttling"
    if r.get("cg_psi_cpu_some", 0) > 0.05:
        return "contention"
    if r.get("psi_io_some", 0) > 0.10:
        return "io"
    return "none"


def rules_host_only(r) -> str:
    """Naive baseline a host-wide monitor would use: CPU% and load only."""
    if r["host_busy"] > 50 or r["load1"] > 8:
        return "contention"
    if r["host_iowait"] > 10:
        return "io"
    return "none"


def analyze():
    import numpy as np
    from sklearn.linear_model import LogisticRegression
    from sklearn.metrics import accuracy_score, f1_score
    from sklearn.neural_network import MLPClassifier
    from sklearn.preprocessing import StandardScaler
    from sklearn.tree import DecisionTreeClassifier, export_text

    rows = load_windows()
    train = [r for r in rows if r["split"] == "train"]
    test = [r for r in rows if r["split"] == "test"]
    base = np.median([r["lat_p95_us"] for r in train if r["label"] == "none" and r["run"].startswith("none")])
    for r in rows:
        r["degraded"] = int(r["lat_p95_us"] > 1.5 * base)

    MECH = ["psi_cpu_some", "psi_mem_some", "psi_io_some", "psi_io_full", "cg_usage_cores", "cg_throttled_ratio",
            "cg_throttled_frac", "cg_psi_cpu_some"]
    feats = FEATURES

    def X(rs):
        return np.array([[float(r.get(f, 0.0)) for f in feats] for r in rs])

    ytr = np.array([CLASSES.index(r["label"]) for r in train])
    yte = np.array([CLASSES.index(r["label"]) for r in test])
    res = {"windows_train": len(train), "windows_test": len(test), "baseline_p95_us": float(base),
           "features": FEATURES, "classes": CLASSES, "models": {}}

    def score(name, pred, extra=None):
        pred = np.asarray(pred)
        res["models"][name] = {"test_acc": float(accuracy_score(yte, pred)),
                               "test_macro_f1": float(f1_score(yte, pred, average="macro")),
                               "per_class_recall": {c: float(np.mean(pred[yte == i] == i)) if np.any(yte == i) else None
                                                    for i, c in enumerate(CLASSES)}, **(extra or {})}

    score("rules_a_priori", [CLASSES.index(rules_a_priori(r)) for r in test], {"params": 3, "train_seconds": 0.0})
    score("rules_host_cpu_only", [CLASSES.index(rules_host_only(r)) for r in test], {"params": 3, "train_seconds": 0.0})

    for suffix, fset in (("", FEATURES), ("_mechanistic", MECH)):
        feats = fset
        sc = StandardScaler().fit(X(train))
        Xtr, Xte = sc.transform(X(train)), sc.transform(X(test))
        for name0, mdl in (("logreg", LogisticRegression(max_iter=2000, C=1.0)),
                           ("tree_d3", DecisionTreeClassifier(max_depth=3, random_state=0)),
                           ("mlp_16", MLPClassifier(hidden_layer_sizes=(16,), max_iter=3000, random_state=0))):
            name = name0 + suffix
            t0 = time.perf_counter()
            mdl.fit(Xtr, ytr)
            tr = time.perf_counter() - t0
            t1 = time.perf_counter()
            for _ in range(200):
                mdl.predict(Xte[:1])
            inf_us = (time.perf_counter() - t1) / 200 * 1e6
            proba = mdl.predict_proba(Xte)
            pred = proba.argmax(1)
            keep = proba.max(1) >= 0.8
            if name0 == "mlp_16":
                params = sum(c.size for c in mdl.coefs_) + sum(b.size for b in mdl.intercepts_)
            elif name0 == "logreg":
                params = mdl.coef_.size + mdl.intercept_.size
            else:
                params = int(mdl.tree_.node_count)
            score(name, pred, {"params": int(params), "features": list(feats), "train_seconds": tr,
                               "python_predict_us": inf_us,
                               "abstain_at_0.8": {"coverage": float(keep.mean()),
                                                  "acc_on_covered": float((pred[keep] == yte[keep]).mean()) if keep.any() else None}})
            if name0 == "tree_d3":
                res["tree_rules" + suffix] = export_text(mdl, feature_names=list(feats))

    # Degradation (outcome) prediction: does an a-priori "bottleneck present" call predict felt latency?
    deg_true = np.array([r["degraded"] for r in test])
    deg_rules = np.array([rules_a_priori(r) != "none" for r in test]).astype(int)
    deg_host = np.array([rules_host_only(r) != "none" for r in test]).astype(int)
    feats = FEATURES
    sc = StandardScaler().fit(X(train))
    Xtr, Xte = sc.transform(X(train)), sc.transform(X(test))
    lr = LogisticRegression(max_iter=2000).fit(Xtr, np.array([r["degraded"] for r in train]))
    res["degradation"] = {"rate_test": float(deg_true.mean()),
                          "rules_a_priori_acc": float((deg_rules == deg_true).mean()),
                          "rules_host_cpu_only_acc": float((deg_host == deg_true).mean()),
                          "logreg_acc": float((lr.predict(Xte) == deg_true).mean())}
    # Per-scenario latency summary (what the user felt).
    lat = {}
    for r in rows:
        lat.setdefault(r["run"], []).append(r["lat_p95_us"])
    res["p95_latency_by_run_us"] = {k: float(np.median(v)) for k, v in sorted(lat.items())}
    os.makedirs(os.path.join(ROOT, "ml/experiments/results"), exist_ok=True)
    json.dump(res, open(os.path.join(ROOT, "ml/experiments/results/cpu_contention.json"), "w"), indent=2)
    for k, v in res["models"].items():
        print(f"{k:<20} acc={v['test_acc']:.3f} macroF1={v['test_macro_f1']:.3f} recall={v['per_class_recall']}")
    print("degradation:", res["degradation"])
    print(res.get("tree_rules", ""))
    print(res.get("tree_rules_mechanistic", ""))


if __name__ == "__main__":
    {"run": run, "analyze": analyze}[sys.argv[1]]()
