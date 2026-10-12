"""Shared dataset code for the CPU world model (docs/WORLD_MODEL_EXPERIMENTS.md).

Features are computed by the C++ encoder (`osie_wm_lab encode`), the same
function OSIE uses at prediction time, so training and inference cannot
disagree about feature definitions.
"""
import json
import math
import os
import subprocess

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WINDOW_FIELDS = ["secs", "usage_cores", "throttled_ratio", "throttled_frac", "psi_some", "psi_full", "quota_cores",
                 "cpuset_cpus", "pids", "host_busy_frac", "host_cpus"]

# Targets: post-intervention means over the POST windows.
OBSERVABLE_TARGETS = {  # what OSIE can observe at run time (cgroup telemetry)
    "usage_cores": ("cores", "usage_cores"),
    "throttled_cores": ("cores", "throttled_frac"),
    "throttled_ratio": ("fraction", "throttled_ratio"),
    "psi_some": ("fraction", "psi_some"),
}
LAB_TARGETS = ["lat_p50_ms", "lat_p99_ms", "completed_per_s"]  # application metrics, lab only

TRAIN_FAMILIES = ["arith", "hash", "short", "burst"]
HELDOUT_FAMILY = "mixed"
HELDOUT_QUOTAS = [1.5, 2.5]  # Experiment 5: never a post-intervention quota in training


def lab_binary():
    for p in [os.path.join(ROOT, "build", "osie_wm_lab"), os.path.join(ROOT, "build-release", "osie_wm_lab")]:
        if os.path.exists(p):
            return p
    raise FileNotFoundError("build osie_wm_lab first (ninja -C build osie_wm_lab)")


def load_episodes(path):
    """Reads episodes.jsonl, or the committed episodes.jsonl.gz next to it."""
    import gzip
    if not os.path.exists(path) and os.path.exists(path + ".gz"):
        with gzip.open(path + ".gz", "rt") as f:
            return [json.loads(l) for l in f]
    return [json.loads(l) for l in open(path)]


def mean_field(windows, key):
    vals = [w[key] for w in windows if w.get(key) is not None]
    return float(np.mean(vals)) if vals else math.nan


def encode(episodes, history=6, binary=None):
    """Feature matrix from the C++ encoder, using the last `history` PRE windows."""
    binary = binary or lab_binary()
    lines = []
    for i, e in enumerate(episodes):
        a = e["action"]
        wins = []
        for w in e["pre"][-history:]:
            wins.append(",".join("nan" if w.get(f) is None else repr(float(w[f])) for f in WINDOW_FIELDS))
        lines.append(f"{i}\t{a['kind']} {a['previous']} {a['proposed']}\t{';'.join(wins)}")
    out = subprocess.run([binary, "encode"], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True).stdout
    names = subprocess.run([binary, "names"], capture_output=True, text=True, check=True).stdout.split()
    X = np.full((len(episodes), len(names)), np.nan)
    ok = np.zeros(len(episodes), dtype=bool)
    for line in out.splitlines():
        idx, status, vals = line.split("\t", 2)
        if status == "OK":
            X[int(idx)] = [float(v) for v in vals.split(",")]
            ok[int(idx)] = True
    return X, names, ok


def targets(episodes):
    Y = {}
    for t, (_, field) in OBSERVABLE_TARGETS.items():
        Y[t] = np.array([mean_field(e["post"], field) for e in episodes])
        Y[t + "_pre"] = np.array([mean_field(e["pre"], field) for e in episodes])
    for t in LAB_TARGETS:
        Y[t] = np.array([mean_field(e["post"], t) for e in episodes])
        Y[t + "_pre"] = np.array([mean_field(e["pre"], t) for e in episodes])
    return Y


def lab_features(episodes):
    """Application-level features available only in the lab (not to OSIE)."""
    names = ["l_offered_cpu", "l_workers", "l_p50_pre", "l_p99_pre", "l_completed_pre", "l_queue_pre"]
    X = []
    for e in episodes:
        cpu_ms = mean_field(e["pre"], "cpu_ms_per_req")
        offered = mean_field(e["pre"], "arrivals_per_s")
        load_f = e["action"]["proposed"] / e["action"]["previous"] if e["action"]["kind"] == "offered_load" else 1.0
        X.append([offered * (cpu_ms if not math.isnan(cpu_ms) else 0) / 1000.0 * load_f, e["config"]["workers"],
                  math.log1p(mean_field(e["pre"], "lat_p50_ms")), math.log1p(mean_field(e["pre"], "lat_p99_ms")),
                  mean_field(e["pre"], "completed_per_s"), math.log1p(e["pre"][-1].get("queue_end") or 0)])
    return np.nan_to_num(np.array(X)), names


def split_runs(episodes, seed=7):
    """Run-level split: per training family 6 train / 2 val / 2 test runs;
    the held-out family is test only. Returns run -> partition."""
    rng = np.random.RandomState(seed)
    runs = sorted({e["run_id"] for e in episodes})
    part = {}
    for fam in TRAIN_FAMILIES:
        fr = [r for r in runs if r.startswith(fam + "-")]
        rng.shuffle(fr)
        n = len(fr)
        n_test = max(1, round(n * 0.2))
        n_val = max(1, round(n * 0.2))
        for r in fr[:n_test]:
            part[r] = "test"
        for r in fr[n_test:n_test + n_val]:
            part[r] = "val"
        for r in fr[n_test + n_val:]:
            part[r] = "train"
    for r in runs:
        if r.startswith(HELDOUT_FAMILY + "-"):
            part[r] = "ood_family"
    return part


def group_cols(names, groups):
    return [i for i, n in enumerate(names) if any(n.startswith(g) for g in groups)]


FEATURE_SETS = {
    # Experiment 1 / 8. Action indicators and raw action values are generic.
    "generic": ["g_", "h_"],
    "mechanism": ["m_", "g_act_none", "g_act_quota", "g_act_cpuset", "g_act_load", "h_"],
    "both": ["g_", "m_", "h_"],
}


def bootstrap_ci(err_by_run, n=2000, seed=0):
    """95% CI of the mean error, resampling independent runs (not windows)."""
    rng = np.random.RandomState(seed)
    runs = list(err_by_run)
    if len(runs) < 2:
        return (math.nan, math.nan)
    means = []
    for _ in range(n):
        pick = rng.choice(len(runs), len(runs), replace=True)
        vals = np.concatenate([err_by_run[runs[i]] for i in pick])
        means.append(vals.mean())
    return (float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5)))


def mae_with_ci(y, p, run_ids):
    err = np.abs(y - p)
    by = {}
    for r, e in zip(run_ids, err):
        by.setdefault(r, []).append(e)
    by = {k: np.array(v) for k, v in by.items()}
    lo, hi = bootstrap_ci(by)
    return {"mae": float(err.mean()), "ci95": [lo, hi], "rmse": float(np.sqrt((err ** 2).mean())),
            "episodes": int(len(y)), "runs": int(len(by))}
