#!/usr/bin/env python3
"""Latency prediction (lab only) and counterfactual intervention ranking.

    python3 ranking.py --data data/v1 --cf data/cf1 --results results

Latency is an application metric that OSIE cannot observe at run time, so
these models are research results only; they are not exported.
"""
import argparse
import json
import math
import os

import numpy as np
from scipy.stats import spearmanr

import experiments as ex
import wm

LAT = "lat_p99_ms"


def prepare(path, extra_filter=None):
    eps = [e for e in wm.load_episodes(path) if e["valid"]]
    if extra_filter:
        eps = [e for e in eps if extra_filter(e)]
    X, names, ok = wm.encode(eps)
    eps, X = [e for e, k in zip(eps, ok) if k], X[ok]
    Y = wm.targets(eps)
    L, lnames = wm.lab_features(eps)
    keep = np.all([np.isfinite(Y[t]) for t in ex.TARGETS + [LAT, LAT + "_pre"]], axis=0)
    keep &= np.all([np.isfinite(Y[t + "_pre"]) for t in ex.TARGETS], axis=0)
    return [e for e, k in zip(eps, keep) if k], X[keep], L[keep], {k: v[keep] for k, v in Y.items()}, names, lnames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--cf", required=True)
    ap.add_argument("--results", required=True)
    args = ap.parse_args()

    eps, X, L, Y, names, lnames = prepare(os.path.join(args.data, "episodes.jsonl"))
    part = wm.split_runs(eps)
    P = np.array([part[e["run_id"]] for e in eps])
    idx = {p: np.where(P == p)[0] for p in ["train", "val", "test", "ood_family"]}
    cols = wm.group_cols(names, wm.FEATURE_SETS["both"])
    gen = wm.group_cols(names, wm.FEATURE_SETS["generic"])
    yl = np.log1p(Y[LAT])[:, None]  # log latency: errors are relative
    tr, va = idx["train"], idx["val"]
    for t in ex.TARGETS:
        ex.SCALE[t] = float(np.std(Y[t][tr])) or 1.0

    feats = {
        "osie_observable(both)": lambda Xs, Ls: Xs[:, cols],
        "generic+lab": lambda Xs, Ls: np.hstack([Xs[:, gen], Ls]),
        "both+lab": lambda Xs, Ls: np.hstack([Xs[:, cols], Ls]),
    }
    lat_models = {}
    out = {"latency": {}}
    print("== p99 latency (log1p ms); MAE in log units ~ relative error")
    nc = {n: float(np.abs(np.log1p(Y[LAT + "_pre"][ix]) - yl[ix, 0]).mean()) for n, ix in idx.items() if n != "train"}
    out["latency"]["no_change"] = nc
    print("no_change", {k: round(v, 3) for k, v in nc.items()})
    for fname, f in feats.items():
        for mname, fit in [("ridge", ex.fit_ridge), ("gbt", ex.fit_gbt), ("mlp_h16", ex.fit_mlp(16))]:
            pred, info = fit(f(X[tr], L[tr]), yl[tr], f(X[va], L[va]), yl[va], seed=1)
            r = {}
            for n in ["val", "test", "ood_family"]:
                ix = idx[n]
                p = np.asarray(pred(f(X[ix], L[ix]))).reshape(len(ix), -1)[:, 0]
                err = np.abs(p - yl[ix, 0])
                by = {}
                for i, e in zip(ix, err):
                    by.setdefault(eps[i]["run_id"], []).append(e)
                ci = wm.bootstrap_ci({k: np.array(v) for k, v in by.items()})
                d_true = yl[ix, 0] - np.log1p(Y[LAT + "_pre"][ix])
                d_pred = p - np.log1p(Y[LAT + "_pre"][ix])
                m = np.abs(d_true) > math.log(1.25)  # >25% change
                r[n] = {"mae_log": float(err.mean()), "ci95": ci,
                        "direction_acc": float((np.sign(d_true[m]) == np.sign(d_pred[m])).mean()) if m.any() else None,
                        "n_direction": int(m.sum())}
            out["latency"][f"{mname}/{fname}"] = dict(r, params=info["params"])
            lat_models[f"{mname}/{fname}"] = (pred, f)
            print(f"{mname}/{fname:22s} val={r['val']['mae_log']:.3f} test={r['test']['mae_log']:.3f} "
                  f"ood={r['ood_family']['mae_log']:.3f} dir(test)={r['test']['direction_acc']}")

    # ---- counterfactual ranking -------------------------------------------
    cf, Xc, Lc, Yc, _, _ = prepare(os.path.join(args.cf, "episodes.jsonl"))
    groups = {}
    for i, e in enumerate(cf):
        groups.setdefault(e["counterfactual_group"], []).append(i)
    groups = {g: v for g, v in groups.items() if len(v) == 4}
    obs_p99 = np.log1p(Yc[LAT])
    print(f"\n== counterfactual ranking: {len(groups)} complete groups of 4 candidate quotas")
    best_lat = min((v["val"]["mae_log"], k) for k, v in out["latency"].items() if k != "no_change")[1]
    rankers = {
        "more_cpu_is_better": lambda ix: -np.array([cf[i]["candidate_quota"] for i in ix]),
        "mechanistic_throttling": lambda ix: ex.mechanistic(Xc[ix], names)[:, 1],
        f"latency_model[{best_lat}]": lambda ix: np.asarray(lat_models[best_lat][0](lat_models[best_lat][1](Xc[ix], Lc[ix]))).reshape(len(ix), -1)[:, 0],
    }
    rank_out = {}
    for rname, score in rankers.items():
        rho, top1, regret, improve = [], [], [], []
        for g, ix in groups.items():
            ix = np.array(ix)
            s = score(ix)
            o = obs_p99[ix]
            if np.ptp(o) > 1e-9 and np.ptp(s) > 1e-9:
                rho.append(spearmanr(s, o).correlation)
            chosen = int(np.argmin(s))
            top1.append(o[chosen] <= o.min() + 1e-9)
            regret.append(float(np.expm1(o[chosen]) - np.expm1(o.min())))
            # "Will raising the quota from 1 to X improve p99?" against the q=1 run
            base = [k for k, i in enumerate(ix) if cf[i]["candidate_quota"] == 1.0][0]
            for k, i in enumerate(ix):
                if cf[i]["candidate_quota"] > 1.0:
                    true_better = o[k] < o[base] - math.log(1.1)
                    pred_better = s[k] < s[base] - 1e-9
                    improve.append(true_better == pred_better)
        rank_out[rname] = {"spearman_mean": float(np.mean(rho)) if rho else None, "top1_acc": float(np.mean(top1)),
                           "median_regret_ms": float(np.median(regret)), "mean_regret_ms": float(np.mean(regret)),
                           "improve_question_acc": float(np.mean(improve)), "groups": len(groups)}
        print(rname, {k: (round(v, 3) if isinstance(v, float) else v) for k, v in rank_out[rname].items()})
    # How often is "more CPU" actually not better? (falsifies the naive assumption where it fails)
    not_better = 0
    for g, ix in groups.items():
        q = np.array([cf[i]["candidate_quota"] for i in ix])
        o = obs_p99[ix]
        if o[np.argmax(q)] > o.min() + math.log(1.1):
            not_better += 1
    rank_out["groups_where_largest_quota_not_best_by_10pct"] = not_better
    rank_out["families"] = sorted({cf[i]["family"] for ix in groups.values() for i in ix})
    out["counterfactual"] = rank_out
    json.dump(out, open(os.path.join(args.results, "latency_ranking.json"), "w"), indent=1, default=float)
    print("wrote", os.path.join(args.results, "latency_ranking.json"))


if __name__ == "__main__":
    main()
