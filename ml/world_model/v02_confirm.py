#!/usr/bin/env python3
"""Confirmatory evaluation of the frozen v0.2 CPU world model. Pre-registered:
this script and the frozen artifact were committed before any confirmatory
data were collected (results/v02/freeze.json has the artifact hash).

    python3 v02_confirm.py --model ../../models/world_cpu.osiewm --data data/confirm-local data/confirm-gha-x86 ... \
        --out results/v02/confirm.json

Every prediction comes from the deployed C++ code (`osie_wm_lab
predict-json`), twice: as the user would get it (with the environment check:
`deployed`) and forced (without it: `forced`), which measures whether the
model would have transferred where it refuses to predict.

Groups: environment x workload group, where the workload group is
  held_in   families of the development data (new runs, new seeds)
  new       families never used before freezing (variable, closed, wide, hashbig)
  batch     closed-loop batch threads
Metrics per group: MAE per target with run-bootstrap CIs and the no-change
baseline; interval coverage and width at 80% / 95%; coverage by regime;
abstention rate and reasons; false-safe throttling rate; clamping rate.
"""
import argparse
import hashlib
import json
import math
import os
import subprocess
from collections import Counter, defaultdict

import numpy as np

import wm

TARGETS = ["usage_cores", "throttled_cores", "throttled_ratio", "psi_some"]
HELD_IN = {"arith", "hash", "short", "burst", "mixed"}
NEW = {"variable", "closed", "wide", "hashbig"}
SAFE_MAX = 0.25  # cores; must equal safe_throttle_max in the artifact


def group_of(e):
    f = e["family"]
    if f.startswith("batch"):
        return "batch"
    return "held_in" if f in HELD_IN else "new" if f in NEW else "other"


def lab_predict(model, eps, arch=None, kernel=None):
    lines = []
    for i, e in enumerate(eps):
        a = e["action"]
        wins = [",".join("nan" if w.get(f) is None else repr(float(w[f])) for f in wm.WINDOW_FIELDS) for w in e["pre"][-6:]]
        lines.append(f"{i}\t{a['kind']} {a['previous']} {a['proposed']}\t{';'.join(wins)}")
    cmd = [wm.lab_binary(), "predict-json", model] + ([arch, kernel] if arch else [])
    out = subprocess.run(cmd, input="\n".join(lines) + "\n", capture_output=True, text=True, check=True).stdout
    res = {}
    for l in out.splitlines():
        o = json.loads(l)
        res[int(o["id"])] = o
    return res


def env_of(d):
    env = json.load(open(os.path.join(d, "environment.json")))
    arch = env.get("architecture", "")
    arch = {"arm64": "aarch64", "amd64": "x86_64"}.get(arch, arch)
    return env, arch, env.get("kernel", "")


def summarize(rows, scale):
    """rows: dicts with y (targets), pre, pred (forced), dep (deployed), run, action, regime."""
    out = {"episodes": len(rows), "runs": len({r["run"] for r in rows})}
    if not rows:
        return out
    for t in TARGETS:
        y = np.array([r["y"][t] for r in rows])
        nc = np.array([r["pre"][t] for r in rows])
        ok = [r for r in rows if r["pred"] is not None]
        p = np.array([r["pred"]["targets"][t]["expected"] for r in ok])
        yo = np.array([r["y"][t] for r in ok])
        by = defaultdict(list)
        for r, e in zip(ok, np.abs(p - yo)):
            by[r["run"]].append(e)
        lo, hi = wm.bootstrap_ci({k: np.array(v) for k, v in by.items()})
        cov = {}
        for k, lvl in enumerate(("0.8", "0.95")):
            ins = [r["pred"]["targets"][t]["intervals"][k][1] - 1e-9 <= r["y"][t] <= r["pred"]["targets"][t]["intervals"][k][2] + 1e-9
                   for r in ok]
            widths = [r["pred"]["targets"][t]["intervals"][k][2] - r["pred"]["targets"][t]["intervals"][k][1] for r in ok]
            cov[lvl] = {"coverage": float(np.mean(ins)), "mean_width": float(np.mean(widths))}
        out[t] = {"mae": float(np.abs(p - yo).mean()), "ci95": [lo, hi], "norm_mae": float(np.abs(p - yo).mean() / scale[t]),
                  "no_change_mae": float(np.abs(nc - y).mean()), "coverage": cov,
                  "clamped_rate": float(np.mean([r["pred"]["targets"][t]["clamped"] for r in ok]))}
    # Deployed behaviour: abstention and its reasons.
    dep = [r for r in rows if r["dep"] is not None]
    out["deployed_abstention_rate"] = float(np.mean([r["dep"]["abstained"] for r in dep])) if dep else None
    reasons = Counter()
    for r in dep:
        for x in r["dep"]["reasons"]:
            reasons[x.split(" (")[0].split(" = ")[0][:70]] += 1
    out["abstention_reasons"] = dict(reasons.most_common(8))
    out["forced_ood_rate"] = float(np.mean([r["pred"]["ood"] for r in rows if r["pred"] is not None]))
    # False-safe throttling: quota/cpuset changes assessed "low" whose observed throttling exceeded SAFE_MAX.
    ch = [r for r in rows if r["action"] in ("cpu_quota", "cpuset") and r["pred"] is not None]
    for mode in ("forced", "deployed"):
        key = "pred" if mode == "forced" else "dep"
        cc = [r for r in ch if r[key] is not None]
        low = [r for r in cc if r[key]["assessment"] == "low"]
        bad = [r for r in low if r["y"]["throttled_cores"] > SAFE_MAX]
        point_low = [r for r in cc if r["pred"]["targets"]["throttled_cores"]["expected"] <= 0.1]
        point_bad = [r for r in point_low if r["y"]["throttled_cores"] > SAFE_MAX]
        out[f"false_safe_{mode}"] = {
            "changes": len(cc), "assessed_low": len(low), "false_safe": len(bad),
            "false_safe_rate_among_low": len(bad) / len(low) if low else None,
            "withheld_or_unknown": sum(r[key]["assessment"] == "unknown" for r in cc),
            "assessments": dict(Counter(r[key]["assessment"] for r in cc)),
            "point_estimate_rule_false_safe": f"{len(point_bad)}/{len(point_low)}",
        }
    # Coverage by regime (throttled_cores, 95%).
    reg = defaultdict(list)
    for r in rows:
        if r["pred"] is None:
            continue
        iv = r["pred"]["targets"]["throttled_cores"]["intervals"][1]
        reg[r["regime"]].append(iv[1] - 1e-9 <= r["y"]["throttled_cores"] <= iv[2] + 1e-9)
    out["throttled_cov95_by_regime"] = {k: {"coverage": float(np.mean(v)), "n": len(v)} for k, v in reg.items()}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=os.path.join(wm.ROOT, "models", "world_cpu.osiewm"))
    ap.add_argument("--data", nargs="+", required=True)
    ap.add_argument("--freeze", default="results/v02/freeze.json")
    ap.add_argument("--out", default="results/v02/confirm.json")
    a = ap.parse_args()
    freeze = json.load(open(a.freeze))
    sha = hashlib.sha256(open(a.model, "rb").read()).hexdigest()
    if sha != freeze["sha256"]:
        raise SystemExit(f"model hash {sha} differs from the frozen artifact {freeze['sha256']}")
    dev_scale = {}
    result = {"model_sha256": sha, "environments": {}}
    for d in a.data:
        env, arch, kernel = env_of(d)
        eps = []
        for sub in ("", "main", "batch"):
            p = os.path.join(d, sub, "episodes.jsonl")
            if os.path.exists(p) or os.path.exists(p + ".gz"):
                eps += [e for e in wm.load_episodes(p) if e["valid"]]
        Y = wm.targets(eps)
        keep = [i for i in range(len(eps)) if all(np.isfinite(Y[t][i]) and np.isfinite(Y[t + "_pre"][i]) for t in TARGETS)]
        eps = [eps[i] for i in keep]
        Y = {k: v[keep] for k, v in Y.items()}
        forced = lab_predict(a.model, eps)
        deployed = lab_predict(a.model, eps, arch, kernel)
        if not dev_scale:
            dev_scale = {t: 1.0 for t in TARGETS}
            try:
                sel = json.load(open("results/v02/dev_selection.json"))
                dev_scale = sel["summary"]["target_scale"]
            except OSError:
                pass
        rows = []
        for i, e in enumerate(eps):
            f, dpl = forced.get(i), deployed.get(i)
            if f is not None and "error" in f:
                f = None
            if dpl is not None and "error" in dpl:
                dpl = None
            pre_u = wm.mean_field(e["pre"], "usage_cores")
            q = wm.mean_field(e["pre"], "quota_cores")
            regime = ("pre_saturated" if np.isfinite(q) and pre_u >= 0.9 * q else "pre_unsaturated") + \
                     ("/throttled_after" if Y["throttled_cores"][i] > SAFE_MAX else "/not_throttled_after")
            rows.append({"y": {t: float(Y[t][i]) for t in TARGETS}, "pre": {t: float(Y[t + "_pre"][i]) for t in TARGETS},
                         "pred": f, "dep": dpl, "run": e["run_id"], "family": e["family"], "group": group_of(e),
                         "action": e["action"]["kind"], "regime": regime})
        res = {"environment": {k: env.get(k) for k in ("kernel", "architecture", "os", "hypervisor", "host_machine",
                                                        "vm_cpus", "cgroup_driver", "collected_at")},
               "all": summarize(rows, dev_scale)}
        for g in ("held_in", "new", "batch"):
            res[g] = summarize([r for r in rows if r["group"] == g], dev_scale)
        res["by_family"] = {fam: summarize([r for r in rows if r["family"] == fam], dev_scale)
                            for fam in sorted({r["family"] for r in rows})}
        result["environments"][os.path.basename(os.path.normpath(d))] = res
        print(f"\n== {d}: kernel {env.get('kernel')} {arch}; {len(rows)} episodes")
        for g in ("held_in", "new", "batch"):
            s = res[g]
            if s.get("episodes", 0) == 0:
                continue
            print(f"  {g:8s} n={s['episodes']:4d} runs={s['runs']:3d} " +
                  " ".join(f"{t[:9]}={s[t]['mae']:.3f}(nc {s[t]['no_change_mae']:.3f}, cov95 {s[t]['coverage']['0.95']['coverage']:.2f})"
                           for t in TARGETS) +
                  f" | deployed abstain {s['deployed_abstention_rate']:.2f}"
                  f" | false-safe forced {s['false_safe_forced']['false_safe']}/{s['false_safe_forced']['assessed_low']}")
    json.dump(result, open(a.out, "w"), indent=1, default=float)
    print("\nwrote", a.out)


if __name__ == "__main__":
    main()
