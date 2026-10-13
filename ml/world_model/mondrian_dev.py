#!/usr/bin/env python3
"""Regime-conditional (Mondrian) conformal calibration, developed on the
development data only.

    python3 mondrian_dev.py --out results/v02/mondrian_dev.json

The regime is known at prediction time: rho = demand estimate after the change
/ CPU capacity after the change (both from the encoder). For the mechanistic
targets nothing was fitted, so the `fit` runs are untouched by calibration and
give an honest coverage estimate for widths computed on the `calib` runs."""
import argparse
import json

import numpy as np

import v02_dev as dv
from v02_freeze import group_conformal

EDGES = [0.0, 0.7, 1.3, np.inf]
NAMES = ["unconstrained(rho<0.7)", "borderline(0.7-1.3)", "bound(rho>=1.3)"]


def regime(X, names):
    d = X[:, names.index("m_demand_after")]
    cap = np.maximum(X[:, names.index("m_cap_after")], 1e-6)
    rho = d / cap
    return np.digitize(rho, EDGES[1:-1]), rho


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/v02/mondrian_dev.json")
    a = ap.parse_args()
    eps, X, names, Yd = dv.load_dev()
    T = dv.TARGETS
    Y = np.stack([Yd[t] for t in T], 1)
    runs = np.array([e["run_id"] for e in eps])
    part = dv.partition(eps)
    fit = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "fit"])
    cal = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "calib"])
    P = dv.mechanistic(X, names)
    cap = X[:, names.index("m_cap_after")]
    P[:, 0] = np.clip(P[:, 0], 0, cap)
    reg, rho = regime(X, names)
    out = {"edges": EDGES[1:-1], "targets": {}}
    for j, t in enumerate(T[:3]):  # mechanistic targets only
        r_cal, r_fit = np.abs(Y[cal, j] - P[cal, j]), np.abs(Y[fit, j] - P[fit, j])
        res = {}
        for lvl in (0.8, 0.95):
            g, _ = group_conformal(r_cal, runs[cal], lvl, seed=17)
            row = {"global_width": g, "global_cov_fit": float((r_fit <= g).mean()), "regimes": {}}
            for k, nm in enumerate(NAMES):
                mc, mf = reg[cal] == k, reg[fit] == k
                if mc.sum() < 5:
                    continue
                w, nruns = group_conformal(r_cal[mc], runs[cal][mc], lvl, seed=17)
                row["regimes"][nm] = {"calib_runs": nruns, "calib_episodes": int(mc.sum()), "width": w,
                                      "cov_fit_mondrian": float((r_fit[mf] <= w).mean()) if mf.any() else None,
                                      "cov_fit_global": float((r_fit[mf] <= g).mean()) if mf.any() else None,
                                      "fit_episodes": int(mf.sum())}
            res[str(lvl)] = row
        out["targets"][t] = res
        for lvl, row in res.items():
            print(f"{t:16s} {lvl}: global w={row['global_width']:.3f} cov(fit)={row['global_cov_fit']:.3f}")
            for nm, rr in row["regimes"].items():
                print(f"    {nm:24s} w={rr['width']:.3f} (calib runs {rr['calib_runs']}, eps {rr['calib_episodes']}) "
                      f"cov(fit) mondrian={rr['cov_fit_mondrian']:.3f} global={rr['cov_fit_global']:.3f} n={rr['fit_episodes']}")
    json.dump(out, open(a.out, "w"), indent=1, default=float)


if __name__ == "__main__":
    main()
