#!/usr/bin/env python3
"""Validates CPU-demand estimators against an independent lab measurement
(docs/WORLD_MODEL_EXPERIMENTS.md, v0.2 section "Demand estimation").

    python3 demand_study.py --data data/v1 data/cf1 data/batch1 --out results/v02/demand_study.json

Ground truth (lab only, never available to OSIE): offered CPU demand =
arrivals/s x measured CPU ms per request (CLOCK_THREAD_CPUTIME_ID, so it
excludes throttled and queueing time), capped by the parallelism the
workload can use (workers, or workers x cpu/(cpu+block) when requests block)
and by the cpuset. For the closed-loop batch class, demand = spinning threads
capped by the cpuset.

Estimators use only what OSIE observes in cgroupfs at run time:
  E0 usage                      lower bound
  E1 usage + throttled_time     v0.1 estimator (throttled_usec summed over CPUs)
  E2 usage + psi_some           one waiting task while stalled (cgroup cpu.pressure)
  E3 usage + min(throttled_time, psi_some x cpuset)
  E4 usage + throttled_time x (1 - idle_share), idle_share from usage/quota
"""
import argparse
import json
import math
import os

import numpy as np

import wm

FAM_SHAPE = {"arith": (2.0, 0.0), "hash": (2.0, 0.0), "short": (0.3, 0.0), "burst": (1.0, 0.0), "mixed": (1.0, 2.0),
             "variable": (1.5, 0.0), "wide": (1.0, 0.0), "hashbig": (2.0, 0.0)}
# "closed" (closed loop) has no offered demand independent of throughput, so it is skipped.


def mean(ws, k):
    v = [w[k] for w in ws if w.get(k) is not None]
    return float(np.mean(v)) if v else math.nan


def truth_runnable(e, ws):
    """Instantaneous runnable demand: with a request backlog every worker is
    runnable (workers x cpu/(cpu+block)); without one, the offered demand."""
    if e["family"] == "batch_spin":
        return truth(e, ws)
    c, b = FAM_SHAPE[e["family"]]
    par = e["config"]["workers"] * c / (c + b)
    vals = []
    for w in ws:
        cpu_ms, arr, cpus = w.get("cpu_ms_per_req"), w.get("arrivals_per_s"), w.get("cpuset_cpus")
        if cpu_ms is None or arr is None or cpus is None:
            continue
        backlog = (w.get("queue_end") or 0) > e["config"]["workers"]
        vals.append(min(par if backlog else arr * cpu_ms / 1000.0, par, cpus))
    return float(np.mean(vals)) if vals else math.nan


def truth(e, ws):
    cpus = mean(ws, "cpuset_cpus")
    if e["family"] == "batch_spin":
        return min(float(e["config"]["batch_threads"]), cpus)
    cpu_ms = mean(ws, "cpu_ms_per_req")
    arr = mean(ws, "arrivals_per_s")
    if not np.isfinite(cpu_ms) or not np.isfinite(arr):
        return math.nan
    c, b = FAM_SHAPE[e["family"]]
    par = e["config"]["workers"] * c / (c + b)
    return min(arr * cpu_ms / 1000.0, par, cpus)


def estimators(ws):
    u = mean(ws, "usage_cores")
    th = mean(ws, "throttled_frac")
    th = 0.0 if not np.isfinite(th) else th
    psi = mean(ws, "psi_some")
    psi = 0.0 if not np.isfinite(psi) else psi
    cpus = mean(ws, "cpuset_cpus")
    q = mean(ws, "quota_cores")
    idle = 0.0 if not np.isfinite(q) or q <= 0 else max(0.0, 1.0 - u / q)
    cap = lambda x: min(x, cpus)
    return {"E0_usage": cap(u), "E1_usage+throttled": cap(u + th), "E2_usage+psi": cap(u + psi),
            "E3_usage+min(thr,psi*cpus)": cap(u + min(th, psi * cpus)), "E4_usage+thr*(1-idle)": cap(u + th * (1 - idle))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rows = []
    for d in a.data:
        for e in wm.load_episodes(os.path.join(d, "episodes.jsonl")):
            if not e["valid"] or e["family"] == "closed":
                continue
            for phase in ("pre", "post"):
                ws = e[phase]
                t = truth(e, ws)
                if not np.isfinite(t):
                    continue
                q = mean(ws, "quota_cores")
                bound = np.isfinite(q) and t > q * 1.02
                rows.append({"run": e["run_id"], "family": e["family"], "phase": phase, "truth": t,
                             "truth_runnable": truth_runnable(e, ws), "bound": bool(bound),
                             "quota": q, "ratio_over_quota": t / q if np.isfinite(q) and q > 0 else math.nan,
                             **estimators(ws)})
    names = [k for k in rows[0] if k.startswith("E")]
    out = {"windows_sets": len(rows), "families": sorted({r["family"] for r in rows}), "by_regime": {}}

    def summarize(sub, key="truth"):
        sub = [r for r in sub if np.isfinite(r[key])]
        res = {"n": len(sub), "runs": len({r["run"] for r in sub}), "truth": key}
        y = np.array([r[key] for r in sub])
        for nme in names:
            p = np.array([r[nme] for r in sub])
            by = {}
            for r, err in zip(sub, np.abs(p - y)):
                by.setdefault(r["run"], []).append(err)
            ci = wm.bootstrap_ci({k: np.array(v) for k, v in by.items()})
            res[nme] = {"mae": float(np.abs(p - y).mean()), "ci95": ci, "bias": float((p - y).mean()),
                        "rel_bias": float(((p - y) / np.maximum(y, 1e-3)).mean())}
        return res

    for regime, f in [("unbound", lambda r: not r["bound"]), ("bound", lambda r: r["bound"]), ("all", lambda r: True)]:
        out["by_regime"][regime] = summarize([r for r in rows if f(r)])
        out["by_regime"][regime + "_vs_runnable"] = summarize([r for r in rows if f(r)], "truth_runnable")
    out["bound_by_family"] = {fam: summarize([r for r in rows if r["bound"] and r["family"] == fam])
                              for fam in out["families"] if any(r["bound"] and r["family"] == fam for r in rows)}
    # Over-subscription level: demand/quota bins, where the estimators diverge.
    out["bound_by_oversubscription"] = {}
    for lo, hi in [(1.0, 1.5), (1.5, 2.5), (2.5, 100)]:
        sub = [r for r in rows if r["bound"] and lo <= r["ratio_over_quota"] < hi]
        if sub:
            out["bound_by_oversubscription"][f"{lo}-{hi}x"] = summarize(sub)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(out, open(a.out, "w"), indent=1)
    for regime in ("unbound", "bound", "bound_vs_runnable"):
        s = out["by_regime"][regime]
        print(f"== {regime}: n={s['n']} runs={s['runs']}")
        for nme in names:
            print(f"  {nme:30s} MAE {s[nme]['mae']:.3f} [{s[nme]['ci95'][0]:.3f},{s[nme]['ci95'][1]:.3f}] bias {s[nme]['bias']:+.3f}")
    print("== bound, by family (MAE)")
    for fam, s in out["bound_by_family"].items():
        print(f"  {fam:11s} n={s['n']:4d} " + " ".join(f"{nme.split('_')[0]}={s[nme]['mae']:.3f}({s[nme]['bias']:+.2f})" for nme in names))
    print("== bound, by demand/quota")
    for k, s in out["bound_by_oversubscription"].items():
        print(f"  {k:9s} n={s['n']:4d} " + " ".join(f"{nme.split('_')[0]}={s[nme]['mae']:.3f}({s[nme]['bias']:+.2f})" for nme in names))


if __name__ == "__main__":
    main()
