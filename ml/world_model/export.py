#!/usr/bin/env python3
"""Export the validation-selected world model to models/world_cpu.osiewm and
check C++/Python parity.

    python3 ml/world_model/export.py --data ml/world_model/data/v1 --results ml/world_model/results
"""
import argparse
import json
import math
import os
import subprocess

import numpy as np

import experiments as ex
import wm

BOUNDS = {"usage_cores": ("cores", "0", "cap"), "throttled_cores": ("cores", "0", "64"),
          "throttled_ratio": ("fraction", "0", "1"), "psi_some": ("fraction", "0", "1")}


def fmt(v):
    return " ".join(repr(float(x)) for x in np.asarray(v).ravel())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--out", default=os.path.join(wm.ROOT, "models", "world_cpu.osiewm"))
    ap.add_argument("--mode", choices=["selected", "mechanistic_psi"], default="mechanistic_psi",
                    help="selected: the validation-selected model; mechanistic_psi: mechanistic usage/throttling "
                         "plus a learned PSI residual (the shipped model, see docs/WORLD_MODEL_RESULTS.md)")
    args = ap.parse_args()
    res = json.load(open(os.path.join(args.results, "experiments.json")))
    choice = res["selection"]["best_exportable"] if args.mode == "selected" else "res_mlp_h16/both"
    mname, fs = choice.split("/")

    eps = [e for e in wm.load_episodes(os.path.join(args.data, "episodes.jsonl")) if e["valid"]]
    X, names, ok = wm.encode(eps)
    eps, X = [e for e, k in zip(eps, ok) if k], X[ok]
    Y = wm.targets(eps)
    keep = np.all([np.isfinite(Y[t]) & np.isfinite(Y[t + "_pre"]) for t in ex.TARGETS], axis=0)
    eps, X = [e for e, k in zip(eps, keep) if k], X[keep]
    Y = {k: v[keep] for k, v in Y.items()}
    Ymat = np.stack([Y[t] for t in ex.TARGETS], 1)
    part = wm.split_runs(eps)
    P = np.array([part[e["run_id"]] for e in eps])
    tr, va, te = (np.where(P == p)[0] for p in ["train", "val", "test"])
    cols = wm.group_cols(names, wm.FEATURE_SETS[fs])

    residual = mname.startswith("res_")
    base = mname[4:] if residual else mname
    fit = {"ridge": ex.fit_ridge, "mlp_h16": ex.fit_mlp(16), "mlp_h128": ex.fit_mlp(128)}.get(base)
    if fit is None:
        raise SystemExit(f"{mname} cannot be exported to the C++ loader")
    mech = ex.mechanistic(X, names) if residual else np.zeros_like(Ymat)
    if args.mode == "mechanistic_psi":
        # Learn only the PSI residual; the other targets keep the mechanistic prior exactly.
        psi = ex.TARGETS.index("psi_some")
        rp1, info = fit(X[tr][:, cols], (Ymat[tr] - mech[tr])[:, [psi]], X[va][:, cols], (Ymat[va] - mech[va])[:, [psi]], seed=1)

        def rp(Z):
            out = np.zeros((Z.shape[0], len(ex.TARGETS)))
            out[:, psi] = np.asarray(rp1(Z)).reshape(len(Z))
            return out
        m1, sy1 = info["model"], info["sy"]
        W2 = np.zeros((len(ex.TARGETS), m1.net[2].weight.shape[1]))
        b2 = np.zeros(len(ex.TARGETS))
        W2[psi] = m1.net[2].weight.detach().numpy()[0] * sy1.sd[0]
        b2[psi] = m1.net[2].bias.detach().numpy()[0] * sy1.sd[0] + sy1.mu[0]
        info["W2_override"], info["b2_override"] = W2, b2
        info["params"] = int(m1.net[0].weight.numel() + m1.net[0].bias.numel() + m1.net[2].weight.numel() + m1.net[2].bias.numel())
        mname = "mech_psi_mlp_h16"
    else:
        rp, info = fit(X[tr][:, cols], Ymat[tr] - mech[tr], X[va][:, cols], Ymat[va] - mech[va], seed=1)
    pred = ex.ResidualPred(rp, cols, names) if residual else rp

    lines = ["OSIEWM1", f"id cpu-world-{mname}-{fs}-v1", f"kind {'linear' if base == 'ridge' else 'mlp'}",
             "history 6", "window_s 0.5", "features " + " ".join(names[c] for c in cols)]
    if residual:
        lines.append("residual_on mechanistic")
    if base == "ridge" and args.mode == "selected":
        sx = info["scaler"]
        lines += ["mean " + fmt(sx.mu), "scale " + fmt(sx.sd), "w2 " + fmt(info["model"].coef_), "b2 " + fmt(info["model"].intercept_)]
    else:
        m, sx, sy = info["model"], info["sx"], info["sy"]
        W1 = m.net[0].weight.detach().numpy()
        b1 = m.net[0].bias.detach().numpy()
        W2 = m.net[2].weight.detach().numpy() * sy.sd[:, None]  # fold the target scaling into the last layer
        b2 = m.net[2].bias.detach().numpy() * sy.sd + sy.mu
        if "W2_override" in info:
            W2, b2 = info["W2_override"], info["b2_override"]
        lines += ["mean " + fmt(sx.mu), "scale " + fmt(sx.sd), f"hidden {W1.shape[0]}", "w1 " + fmt(W1), "b1 " + fmt(b1),
                  "w2 " + fmt(W2), "b2 " + fmt(b2)]
    for t in ex.TARGETS:
        unit, lo, hi = BOUNDS[t]
        lines.append(f"target {t} {unit} identity {lo} {hi}")
    # Split-conformal half-widths on validation residuals of exactly this model.
    Pva = pred(X[va][:, cols])
    for j, t in enumerate(ex.TARGETS):
        r = np.abs(Ymat[va][:, j] - Pva[:, j])
        n = len(r)
        for lvl in (0.8, 0.95):
            lines.append(f"conformal {t} {lvl} {float(np.quantile(r, min(1.0, math.ceil((n + 1) * lvl) / n))):.6g}")
        # Abstain when the primary (80%) interval is wider than half the target's training range.
        rng_t = float(np.ptp(Ymat[tr][:, j]))
        lines.append(f"abstain_width {t} {max(1e-3, 0.5 * rng_t):.6g}")
    ood = res["export"]["ood"] if res["export"]["model"] == choice else None
    Ztr = X[tr][:, cols]
    lo_, hi_ = Ztr.min(0), Ztr.max(0)
    margin = 0.05 * (hi_ - lo_) + 1e-6
    std = ex.Standardizer().fit(Ztr)
    thr = float(np.quantile(((std(X[va][:, cols])) ** 2).mean(1), 0.99))
    lines += ["ood_lo " + fmt(lo_ - margin), "ood_hi " + fmt(hi_ + margin), f"ood_score_max {thr:.6g}"]
    q = [e["action"] for e in eps if e["run_id"] in {eps[i]["run_id"] for i in tr}]
    qa = [a["proposed"] for a in q if a["kind"] == "cpu_quota"] + [a["previous"] for a in q if a["kind"] == "cpu_quota"]
    cs = [a["proposed"] for a in q if a["kind"] == "cpuset"] + [a["previous"] for a in q if a["kind"] == "cpuset"]
    lf = [a["proposed"] for a in q if a["kind"] == "offered_load"]
    lines += ["action none 0 0", f"action cpu_quota {min(qa)} {max(qa)}", f"action cpuset {min(cs)} {max(cs)}",
              f"action offered_load {min(lf)} {max(lf)}"]
    env = json.load(open(os.path.join(args.data, "environment.json")))
    runs_tr = len({eps[i]["run_id"] for i in tr})
    lines.append(f"provenance {runs_tr} runs / {len(tr)} episodes of 4 synthetic workload families in a "
                 f"Linux {env['kernel']} {env['architecture']} Docker VM ({env['host_machine']}); "
                 f"quota {min(qa)}-{max(qa)} cores, cpuset {int(min(cs))}-{int(max(cs))} CPUs")
    lines.append(f"meta mode {args.mode}; params {info['params']}")
    lines.append("end")
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", args.out, f"({info['params']} parameters, features={fs})")

    # ---- parity: C++ predict vs Python on the test runs --------------------
    binary = wm.lab_binary()
    tsv = []
    for i in te:
        e = eps[i]
        a = e["action"]
        wins = [",".join("nan" if w.get(f) is None else repr(float(w[f])) for f in wm.WINDOW_FIELDS) for w in e["pre"][-6:]]
        tsv.append(f"{i}\t{a['kind']} {a['previous']} {a['proposed']}\t{';'.join(wins)}")
    out = subprocess.run([binary, "predict", args.out], input="\n".join(tsv) + "\n", capture_output=True, text=True,
                         check=True).stdout
    Pte = pred(X[te][:, cols])
    cap = X[te][:, names.index("m_cap_after")]
    diffs, abst = [], 0
    for line in out.splitlines():
        i, status, vals = line.split("\t", 2)
        if status == "ERROR":
            continue
        abst += status == "ABSTAIN"
        k = list(te).index(int(i))
        cpp = np.array([float(v) for v in vals.split(",")])
        py = Pte[k].copy()
        py[0] = min(max(py[0], 0), cap[k])
        py[1] = min(max(py[1], 0), 64)
        py[2:] = np.clip(py[2:], 0, 1)
        diffs.append(np.abs(cpp - py).max())
    print(f"parity on {len(diffs)} test episodes: max |C++ - Python| = {max(diffs):.2e}; abstained {abst}")
    json.dump({"model": mname, "mode": args.mode, "params": info["params"], "parity_max_abs_diff": float(max(diffs)),
               "parity_episodes": len(diffs), "abstained_on_test": abst},
              open(os.path.join(args.results, "export.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
