#!/usr/bin/env python3
"""Why learned corrections fail on the batch class (development data only).

    python3 failure_analysis.py --out results/v02/failure_analysis.json

Mean usage residual (observed - mechanism) after quota changes from a
throttled state, by workload family and direction of the change."""
import argparse
import json

import numpy as np

import v02_dev as dv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/v02/failure_analysis.json")
    a = ap.parse_args()
    eps, X, names, Yd = dv.load_dev()
    mech = dv.mechanistic(X, names)
    Y = np.stack([Yd[t] for t in dv.TARGETS], 1)
    c = {n: X[:, i] for i, n in enumerate(names)}
    quota = c["g_act_quota"] > 0
    raise_q = quota & (c["g_act_proposed"] > c["g_quota"] + 1e-6)
    lower_q = quota & (c["g_act_proposed"] < c["g_quota"] - 1e-6)
    throttled_before = c["g_throttled_ratio"] > 0.2
    fam = np.array([dv.family_of(e) for e in eps])
    out = {}
    for f in sorted(set(fam)):
        for nm, m in [("raise", raise_q), ("lower", lower_q)]:
            sel = (fam == f) & m & throttled_before
            if sel.sum() < 3:
                continue
            r = Y[sel, 0] - mech[sel, 0]
            out[f"{f}/quota_{nm}"] = {"episodes": int(sel.sum()), "mean_usage_residual_cores": float(r.mean()),
                                      "share_below_minus_0.1": float((r < -0.1).mean())}
            print(f"{f:7s} quota {nm}: {r.mean():+.3f} cores over {sel.sum()} episodes")
    json.dump(out, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
