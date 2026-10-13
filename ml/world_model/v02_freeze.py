#!/usr/bin/env python3
"""Freezes the v0.2 CPU world model selected by v02_dev.py.

    python3 v02_freeze.py --selection results/v02/dev_selection.json --out ../../models/world_cpu.osiewm \
        --residual-targets psi_some

Fitting uses the `fit` partition only; the residual gate is set on a
run-held-out part of it. The OOD score threshold and the conformal interval
half-widths are computed on the `calib` partition, which no earlier step has
read. Conformal half-widths are *group-aware*: one episode is sampled per
calibration run (episodes of a run are correlated), the finite-sample
quantile is taken over runs, and this is repeated; the median is used.

Writes the OSIEWM2 artifact and results/v02/freeze.json (hashes,
partitions, every setting), and checks C++/Python parity on the calibration
runs with `osie_wm_lab predict-json`.
"""
import argparse
import hashlib
import json
import math
import os
import subprocess
from datetime import datetime, timezone

import numpy as np

import v02_dev as dv
import wm

BOUNDS = {"usage_cores": ("cores", "0", "cap"), "throttled_cores": ("cores", "0", "64"),
          "throttled_ratio": ("fraction", "0", "1"), "psi_some": ("fraction", "0", "1")}
LEVELS = (0.8, 0.95)


def fmt(v):
    return " ".join(repr(float(x)) for x in np.asarray(v).ravel())


def group_conformal(resid, run_ids, level, draws=500, seed=0, return_valid=False):
    """Finite-sample split-conformal quantile over runs, one episode per run.

    With n runs the quantile needs rank ceil((n + 1) * level) <= n. With fewer
    runs no finite width has the guarantee; the largest residual of the
    stratum is then used as a conservative fallback and flagged."""
    rng = np.random.RandomState(seed)
    by = {}
    for r, e in zip(run_ids, resid):
        by.setdefault(r, []).append(e)
    runs = list(by)
    n = len(runs)
    k = math.ceil((n + 1) * level)  # rank of the order statistic
    valid = k <= n
    if not valid:
        q = float(np.max(resid))
        return (q, n, False) if return_valid else (q, n)
    qs = []
    for _ in range(draws):
        s = np.sort([by[r][rng.randint(len(by[r]))] for r in runs])
        qs.append(s[k - 1])
    q = float(np.median(qs))
    return (q, n, True) if return_valid else (q, n)


def tsv_lines(eps, idx):
    out = []
    for i in idx:
        e = eps[i]
        a = e["action"]
        wins = [",".join("nan" if w.get(f) is None else repr(float(w[f])) for f in wm.WINDOW_FIELDS) for w in e["pre"][-6:]]
        out.append(f"{i}\t{a['kind']} {a['previous']} {a['proposed']}\t{';'.join(wins)}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selection", default="results/v02/dev_selection.json")
    ap.add_argument("--out", default=os.path.join(wm.ROOT, "models", "world_cpu.osiewm"))
    ap.add_argument("--record", default="results/v02/freeze.json")
    ap.add_argument("--residual-targets", default="psi_some", help="targets that get a learned, gated residual")
    ap.add_argument("--hidden", type=int, default=8)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--wd", type=float, default=1e-4)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--loss", default="mae")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--safe-throttle-max", type=float, default=0.25)
    ap.add_argument("--no-gate", action="store_true", help="apply the residual everywhere (no residual_gate line)")
    ap.add_argument("--mondrian", action="store_true",
                    help="regime-conditional half-widths for the mechanistic targets (rho edges 0.7, 1.3)")
    a = ap.parse_args()

    sel = json.load(open(a.selection))
    eps, X, names, Yd = dv.load_dev()
    T = dv.TARGETS
    Y = np.stack([Yd[t] for t in T], 1)
    runs = [e["run_id"] for e in eps]
    part = dv.partition(eps)
    fit = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "fit"])
    cal = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "calib"])
    mech = dv.mechanistic(X, names)
    res_t = [t for t in a.residual_targets.split(",") if t]
    rj = [T.index(t) for t in res_t]

    # ---- residual model on `fit` (inner early-stopping split by run) --------
    W1 = b1 = None
    W2 = np.zeros((len(T), a.hidden))
    b2 = np.zeros(len(T))
    gates = {}
    sx = dv.Std().fit(X[fit])
    params = 0
    if rj:
        fi, va = dv.inner_split(fit, runs, a.seed)
        R = (Y - mech)[:, rj]
        p = dv.fit_mlp(X[fi], R[fi], a.hidden, a.lr, a.wd, a.batch, a.loss, a.seed, Xva=X[va], Yva=R[va])
        m, psx, psy = p.torch
        sx = psx  # the residual model's own standardiser is the artifact's
        W1 = m[0].weight.detach().numpy()
        b1 = m[0].bias.detach().numpy()
        w2 = m[2].weight.detach().numpy() * psy.sd[:, None]
        bb = m[2].bias.detach().numpy() * psy.sd + psy.mu
        for k, j in enumerate(rj):
            W2[j] = w2[k]
            b2[j] = bb[k]
        # Gate: the 90th percentile of |residual| on the run-held-out part of `fit`.
        rv = np.abs(p(X[va]))
        if not a.no_gate:
            for k, t in enumerate(res_t):
                gates[t] = float(np.quantile(rv[:, k], 0.9))
        params = int(W1.size + b1.size + w2.size + bb.size)

    def forward(Z):
        z = (Z - sx.mu) / sx.sd
        if W1 is None:
            return np.zeros((len(Z), len(T)))
        h = np.maximum(0, z @ W1.T + b1)
        return h @ W2.T + b2

    # ---- OOD statistics: ranges from `fit`, score threshold from `calib` -----
    lo, hi = X[fit].min(0), X[fit].max(0)
    margin = 0.05 * (hi - lo) + 1e-6
    score = lambda Z: (((Z - sx.mu) / sx.sd) ** 2).mean(1)
    ood_thr = float(np.quantile(score(X[cal]), 0.99))

    def ood(Z):
        return ((Z < lo - margin) | (Z > hi + margin)).any(1) | (score(Z) > ood_thr)

    def predict_py(idx):
        raw = forward(X[idx])
        P = mech[idx].copy()
        o = ood(X[idx])
        for t in res_t:
            j = T.index(t)
            use = ((~o) & (np.abs(raw[:, j]) <= gates[t])) if t in gates else np.ones(len(idx), dtype=bool)
            P[:, j] = np.where(use, P[:, j] + raw[:, j], P[:, j])
        cap = X[idx][:, names.index("m_cap_after")]
        P[:, 0] = np.clip(P[:, 0], 0, cap)
        P[:, 1] = np.clip(P[:, 1], 0, 64)
        P[:, 2:] = np.clip(P[:, 2:], 0, 1)
        return P, o

    # ---- conformal on `calib` -------------------------------------------------
    Pc, oc = predict_py(cal)
    conformal, naive = {}, {}
    for j, t in enumerate(T):
        r = np.abs(Y[cal, j] - Pc[:, j])
        conformal[t] = {}
        naive[t] = {}
        for lvl in LEVELS:
            q, n_runs = group_conformal(r, [runs[i] for i in cal], lvl, seed=17)
            conformal[t][lvl] = q
            n = len(r)
            naive[t][lvl] = float(np.quantile(r, min(1.0, math.ceil((n + 1) * lvl) / n)))

    # ---- support envelope from the development data ----------------------------
    env = json.load(open(os.path.join("data/v1", "environment.json")))
    arch = {"aarch64": "aarch64", "x86_64": "x86_64", "arm64": "aarch64"}.get(env["architecture"], env["architecture"])
    kv = ".".join(env["kernel"].split(".")[:2])
    caps = X[fit][:, names.index("m_cap_after")]
    fams = sorted({dv.family_of(eps[i]) for i in fit})
    acts = [eps[i]["action"] for i in fit]
    qa = [x["proposed"] for x in acts if x["kind"] == "cpu_quota"] + [x["previous"] for x in acts if x["kind"] == "cpu_quota"]
    cs = [x["proposed"] for x in acts if x["kind"] == "cpuset"] + [x["previous"] for x in acts if x["kind"] == "cpuset"]
    lf = [x["proposed"] for x in acts if x["kind"] == "offered_load"]

    model_id = "cpu-world-v0.2-mech" + ("-" + "-".join(f"{t}_res_h{a.hidden}" for t in res_t) if res_t else "")
    L = ["OSIEWM2", f"id {model_id}", f"kind {'mlp' if W1 is not None else 'linear'}", "history 6", "window_s 0.5",
         "features " + " ".join(names), "residual_on mechanistic", "mean " + fmt(sx.mu), "scale " + fmt(sx.sd)]
    if W1 is not None:
        L += [f"hidden {a.hidden}", "w1 " + fmt(W1), "b1 " + fmt(b1), "w2 " + fmt(W2), "b2 " + fmt(b2)]
    else:
        L += ["w2 " + fmt(np.zeros((len(T), len(names)))), "b2 " + fmt(np.zeros(len(T)))]
    for t in T:
        unit, blo, bhi = BOUNDS[t]
        L.append(f"target {t} {unit} identity {blo} {bhi}")
        for lvl in LEVELS:
            L.append(f"conformal {t} {lvl} {conformal[t][lvl]:.6g}")
        L.append(f"abstain_width {t} {max(1e-3, 0.5 * float(np.ptp(Y[fit][:, T.index(t)]))):.6g}")
    regime_rec = {}
    if a.mondrian:
        edges = [(0.0, 0.7), (0.7, 1.3), (1.3, math.inf)]
        rho = X[cal][:, names.index("m_demand_after")] / np.maximum(X[cal][:, names.index("m_cap_after")], 1e-6)
        cal_runs = np.array([runs[i] for i in cal])
        for j, t in enumerate(T):
            if t in res_t:
                continue  # residual targets keep global widths
            r = np.abs(Y[cal, j] - Pc[:, j])
            for lvl in LEVELS:
                for lo_, hi_ in edges:
                    m = (rho >= lo_) & (rho < hi_)
                    if m.sum() == 0:
                        continue
                    w, n_runs, ok = group_conformal(r[m], cal_runs[m], lvl, seed=17, return_valid=True)
                    regime_rec[f"{t}/{lvl}/{lo_}-{hi_}"] = {"width": w, "runs": n_runs, "episodes": int(m.sum()),
                                                            "finite_sample_valid": ok}
                    hi_s = "inf" if math.isinf(hi_) else f"{hi_:g}"
                    L.append(f"conformal_regime {t} {lvl} {lo_:g} {hi_s} {w:.6g}")
    for t in res_t:
        if t in gates:
            L.append(f"residual_gate {t} {gates[t]:.6g}")
        L.append(f"target_status {t} experimental")
    L += ["ood_lo " + fmt(lo - margin), "ood_hi " + fmt(hi + margin), f"ood_score_max {ood_thr:.6g}",
          "action none 0 0", f"action cpu_quota {min(qa)} {max(qa)}", f"action cpuset {min(cs)} {max(cs)}",
          f"action offered_load {min(lf)} {max(lf)}",
          f"feature_schema 1", "model_version wm-0.2.0", "data_version dev-v1_cf1_batch1",
          f"calibration_version group-conformal-{dv.PARTITION_SEED}",
          f"support_arch {arch}", f"support_kernel {kv} {kv}", "support_cgroup 2",
          "support_throttle_semantics summed_per_cpu", f"support_capacity {caps.min():.3g} {caps.max():.3g}",
          "workload_coverage " + " ".join(fams), f"safe_throttle_max {a.safe_throttle_max}",
          f"provenance {len({runs[i] for i in fit})} fit runs + {len({runs[i] for i in cal})} calibration runs of "
          f"{len(fams)} synthetic workload classes; Linux {env['kernel']} {env['architecture']} Docker Desktop VM only",
          f"meta selection {os.path.basename(a.selection)}; residual params {params}", "end"]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    text = "\n".join(L) + "\n"
    open(a.out, "w").write(text)
    sha = hashlib.sha256(text.encode()).hexdigest()
    print(f"wrote {a.out} ({params} residual parameters) sha256 {sha}")

    # ---- C++/Python parity on the calibration runs -------------------------
    out = subprocess.run([wm.lab_binary(), "predict-json", a.out], input="\n".join(tsv_lines(eps, cal)) + "\n",
                         capture_output=True, text=True, check=True).stdout
    cpp = {int(o["id"]): o for o in map(json.loads, out.splitlines())}
    diffs = []
    for k, i in enumerate(cal):
        o = cpp.get(int(i))
        if o is None or "error" in o:
            continue
        for j, t in enumerate(T):
            diffs.append(abs(o["targets"][t]["expected"] - Pc[k, j]))
    parity = float(max(diffs)) if diffs else math.nan
    print(f"C++/Python parity on {len(cal)} calibration episodes: max |diff| = {parity:.2e}")

    rec = {
        "frozen_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "artifact": os.path.relpath(a.out, wm.ROOT), "sha256": sha, "model_id": model_id,
        "residual_targets": res_t, "residual_params": params, "residual_gates": gates,
        "hyperparameters": {"hidden": a.hidden, "lr": a.lr, "wd": a.wd, "batch": a.batch, "loss": a.loss, "seed": a.seed},
        "safe_throttle_max": a.safe_throttle_max, "ood_score_threshold_calib_q99": ood_thr,
        "ood_flag_rate_calib": float(oc.mean()),
        "conformal_group_aware": conformal, "conformal_naive_episode_level": naive,
        "conformal_regime": regime_rec,
        "calibration_runs": sorted({runs[i] for i in cal}), "fit_runs": sorted({runs[i] for i in fit}),
        "parity_max_abs_diff": parity, "selection_file": a.selection,
        "selection_summary": {k: sel["gating"]["policies"][k]["core"] for k in sel["gating"]["policies"]},
    }
    json.dump(rec, open(a.record, "w"), indent=1, default=float)
    print("conformal (group-aware vs naive):", {t: {l: (round(conformal[t][l], 3), round(naive[t][l], 3)) for l in LEVELS} for t in T})


if __name__ == "__main__":
    main()
