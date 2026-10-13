#!/usr/bin/env python3
"""Model C: model B's design, unchanged, with the host-size defect (W8) fixed
and the data of the first confirmatory round used for fitting and
calibration (they are now spent as test data).

    python3 v02_freeze_c.py --out results/v02/world_cpu_v02c.osiewm --record results/v02/freeze_c.json

No selection is made here. The design is B's:
- mechanistic usage, throttled time and throttled ratio;
- a CPU-pressure residual (8 hidden units, quantile loss, lr 3e-3, wd 1e-4,
  batch 32, seed 1);
- group-aware regime-conditional conformal half-widths (rho edges 0.7, 1.3).

Changes:
1. `g_host_cpus` is not a model input and not range-checked. It was
   constant (12) in all earlier data, so every other host was out of
   distribution.
2. Data: development + confirmatory round 1 (local Docker VM, GitHub x86-64
   runner, GitHub arm64 runner). Runs are split 70/30 into fit/calib,
   stratified by (environment, workload class).
3. Support envelope: the architectures and kernel range those data cover.
The confirmatory data for C are collected after C is frozen
(v02_confirm.py on new runs).
"""
import argparse
import hashlib
import json
import math
import os
import random
import subprocess
from datetime import datetime, timezone

import numpy as np

import v02_dev as dv
import wm
from v02_freeze import BOUNDS, LEVELS, fmt, group_conformal, tsv_lines

SETS = ["data/v1", "data/cf1", "data/batch1",
        "data/confirm-local/main", "data/confirm-local/batch",
        "data/gha/wm-ubuntu-24.04/main", "data/gha/wm-ubuntu-24.04/batch",
        "data/gha/wm-ubuntu-24.04-arm/main", "data/gha/wm-ubuntu-24.04-arm/batch"]
DROP = ["g_host_cpus"]
SEED = 20261014


def env_of(path):
    e = json.load(open(os.path.join(path, "environment.json")))
    arch = {"arm64": "aarch64", "amd64": "x86_64"}.get(e["architecture"], e["architecture"])
    return arch, ".".join(e["kernel"].split(".")[:2]), e["kernel"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/v02/world_cpu_v02c.osiewm")
    ap.add_argument("--record", default="results/v02/freeze_c.json")
    a = ap.parse_args()
    eps = []
    for d in SETS:
        arch, kmm, kfull = env_of(d)
        for e in wm.load_episodes(os.path.join(d, "episodes.jsonl")):
            if e["valid"]:
                eps.append(dict(e, env=f"{arch}/{kmm}", kernel=kmm, arch=arch))
    X, names, ok = wm.encode(eps)
    eps, X = [e for e, k in zip(eps, ok) if k], X[ok]
    Yd = wm.targets(eps)
    T = dv.TARGETS
    keep = np.all([np.isfinite(Yd[t]) & np.isfinite(Yd[t + "_pre"]) for t in T], axis=0)
    eps, X = [e for e, k in zip(eps, keep) if k], X[keep]
    Y = np.stack([Yd[t][keep] for t in T], 1)
    runs = [e["run_id"] for e in eps]
    # Partition by (environment, workload class).
    rng = random.Random(SEED)
    strata = {}
    for e in eps:
        strata.setdefault((e["env"], dv.family_of(e)), set()).add(e["run_id"])
    part = {}
    for key in sorted(strata):
        rs = sorted(strata[key])
        rng.shuffle(rs)
        k = max(1, round(len(rs) * 0.3))
        for r in rs[:k]:
            part[r] = "calib"
        for r in rs[k:]:
            part[r] = "fit"
    fit = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "fit"])
    cal = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "calib"])
    cols = [i for i, n in enumerate(names) if n not in DROP]
    mech = dv.mechanistic(X, names)

    # ---- CPU-pressure residual (B's configuration) ---------------------------
    psi = T.index("psi_some")
    fi, va = dv.inner_split(fit, runs, 1)
    R = (Y - mech)[:, [psi]]
    p = dv.fit_mlp(X[fi][:, cols], R[fi], 8, 3e-3, 1e-4, 32, "quantile", 1, Xva=X[va][:, cols], Yva=R[va])
    m, sx, sy = p.torch
    W1 = m[0].weight.detach().numpy()
    b1 = m[0].bias.detach().numpy()
    W2 = np.zeros((len(T), 8))
    b2 = np.zeros(len(T))
    W2[psi] = m[2].weight.detach().numpy()[0] * sy.sd[0]
    b2[psi] = m[2].bias.detach().numpy()[0] * sy.sd[0] + sy.mu[0]

    Z = X[:, cols]
    lo, hi = Z[fit].min(0), Z[fit].max(0)
    margin = 0.05 * (hi - lo) + 1e-6
    score = lambda Q: (((Q - sx.mu) / sx.sd) ** 2).mean(1)
    ood_thr = float(np.quantile(score(Z[cal]), 0.99))

    def predict_py(idx):
        h = np.maximum(0, ((Z[idx] - sx.mu) / sx.sd) @ W1.T + b1)
        P = mech[idx].copy()
        P[:, psi] = P[:, psi] + (h @ W2.T + b2)[:, psi]
        cap = X[idx][:, names.index("m_cap_after")]
        P[:, 0] = np.clip(P[:, 0], 0, cap)
        P[:, 1] = np.clip(P[:, 1], 0, 64)
        P[:, 2:] = np.clip(P[:, 2:], 0, 1)
        return P

    Pc = predict_py(cal)
    cal_runs = np.array([runs[i] for i in cal])
    conformal, regimes = {}, {}
    rho = X[cal][:, names.index("m_demand_after")] / np.maximum(X[cal][:, names.index("m_cap_after")], 1e-6)
    L_reg = []
    for j, t in enumerate(T):
        r = np.abs(Y[cal, j] - Pc[:, j])
        conformal[t] = {}
        for lvl in LEVELS:
            conformal[t][lvl], _ = group_conformal(r, cal_runs, lvl, seed=17)
            if t == "psi_some":
                continue
            for lo_, hi_ in [(0.0, 0.7), (0.7, 1.3), (1.3, math.inf)]:
                msk = (rho >= lo_) & (rho < hi_)
                if not msk.any():
                    continue
                w, n_runs, valid = group_conformal(r[msk], cal_runs[msk], lvl, seed=17, return_valid=True)
                regimes[f"{t}/{lvl}/{lo_}-{hi_}"] = {"width": w, "runs": n_runs, "episodes": int(msk.sum()),
                                                     "finite_sample_valid": valid}
                L_reg.append(f"conformal_regime {t} {lvl} {lo_:g} {'inf' if math.isinf(hi_) else f'{hi_:g}'} {w:.6g}")

    archs = sorted({e["arch"] for e in eps})
    kernels = sorted({tuple(int(x) for x in e["kernel"].split(".")) for e in eps})
    caps = X[fit][:, names.index("m_cap_after")]
    fams = sorted({dv.family_of(eps[i]) for i in fit})
    acts = [eps[i]["action"] for i in fit]
    qa = [x["proposed"] for x in acts if x["kind"] == "cpu_quota"] + [x["previous"] for x in acts if x["kind"] == "cpu_quota"]
    cs = [x["proposed"] for x in acts if x["kind"] == "cpuset"] + [x["previous"] for x in acts if x["kind"] == "cpuset"]
    lf = [x["proposed"] for x in acts if x["kind"] == "offered_load"]
    L = ["OSIEWM2", "id cpu-world-v0.2c-mech-psi_some_res_h8", "kind mlp", "history 6", "window_s 0.5",
         "features " + " ".join(names[c] for c in cols), "residual_on mechanistic", "mean " + fmt(sx.mu),
         "scale " + fmt(sx.sd), "hidden 8", "w1 " + fmt(W1), "b1 " + fmt(b1), "w2 " + fmt(W2), "b2 " + fmt(b2)]
    for t in T:
        unit, blo, bhi = BOUNDS[t]
        L.append(f"target {t} {unit} identity {blo} {bhi}")
        for lvl in LEVELS:
            L.append(f"conformal {t} {lvl} {conformal[t][lvl]:.6g}")
        L.append(f"abstain_width {t} {max(1e-3, 0.5 * float(np.ptp(Y[fit][:, T.index(t)]))):.6g}")
    L += L_reg + ["target_status psi_some experimental",
                  "ood_lo " + fmt(lo - margin), "ood_hi " + fmt(hi + margin), f"ood_score_max {ood_thr:.6g}",
                  "action none 0 0", f"action cpu_quota {min(qa)} {max(qa)}", f"action cpuset {min(cs)} {max(cs)}",
                  f"action offered_load {min(lf)} {max(lf)}", "feature_schema 1", "model_version wm-0.2.0-c",
                  "data_version dev_v1_cf1_batch1-confirm1_local_gha", f"calibration_version group-mondrian-{SEED}",
                  "support_arch " + " ".join(archs),
                  f"support_kernel {kernels[0][0]}.{kernels[0][1]} {kernels[-1][0]}.{kernels[-1][1]}",
                  "support_cgroup 2", "support_throttle_semantics summed_per_cpu",
                  f"support_capacity {caps.min():.3g} {caps.max():.3g}", "workload_coverage " + " ".join(fams),
                  "safe_throttle_max 0.25",
                  f"provenance {len({runs[i] for i in fit})} fit + {len({runs[i] for i in cal})} calibration runs, "
                  f"{len(fams)} synthetic workload classes, Linux {kernels[0][0]}.{kernels[0][1]}-"
                  f"{kernels[-1][0]}.{kernels[-1][1]} on {' and '.join(archs)} (Docker Desktop VM; GitHub-hosted VMs)",
                  "meta model B design; g_host_cpus dropped (W8)", "end"]
    text = "\n".join(L) + "\n"
    open(a.out, "w").write(text)
    sha = hashlib.sha256(text.encode()).hexdigest()
    out = subprocess.run([wm.lab_binary(), "predict-json", a.out], input="\n".join(tsv_lines(eps, cal)) + "\n",
                         capture_output=True, text=True, check=True).stdout
    cpp = {int(o["id"]): o for o in map(json.loads, out.splitlines())}
    diffs = [abs(cpp[int(i)]["targets"][t]["expected"] - Pc[k, j]) for k, i in enumerate(cal) if int(i) in cpp and
             "error" not in cpp[int(i)] for j, t in enumerate(T)]
    rec = {"frozen_at": datetime.now(timezone.utc).isoformat(timespec="seconds"), "artifact": a.out, "sha256": sha,
           "fit_runs": len({runs[i] for i in fit}), "calib_runs": len({runs[i] for i in cal}),
           "environments": sorted({e["env"] for e in eps}), "dropped_features": DROP,
           "conformal": conformal, "conformal_regime": regimes, "ood_score_threshold": ood_thr,
           "parity_max_abs_diff": float(max(diffs)), "support": {"arch": archs, "kernels": [".".join(map(str, k)) for k in kernels]}}
    json.dump(rec, open(a.record, "w"), indent=1, default=float)
    print(f"wrote {a.out} sha256 {sha}; fit runs {rec['fit_runs']}, calib runs {rec['calib_runs']}; parity {rec['parity_max_abs_diff']:.1e}")
    print("invalid strata:", {k: v for k, v in regimes.items() if not v["finite_sample_valid"]})


if __name__ == "__main__":
    main()
