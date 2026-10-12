#!/usr/bin/env python3
"""Scores the exported C++ model (models/world_cpu.osiewm) end to end, with its
physical-bound clamping and abstention, against the mechanistic baseline.

    python3 deployed_eval.py --data data/v1 --cf data/cf1 --results results
"""
import argparse
import json
import os
import subprocess

import numpy as np

import experiments as ex
import wm


def cpp(binary, model, eps, mode="predict"):
    tsv = []
    for i, e in enumerate(eps):
        a = e["action"]
        wins = [",".join("nan" if w.get(f) is None else repr(float(w[f])) for f in wm.WINDOW_FIELDS) for w in e["pre"][-6:]]
        tsv.append(f"{i}\t{a['kind']} {a['previous']} {a['proposed']}\t{';'.join(wins)}")
    return subprocess.run([binary, mode, model], input="\n".join(tsv) + "\n", capture_output=True, text=True, check=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--cf", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--batch", default=None, help="held-out workload class (closed-loop batch threads)")
    ap.add_argument("--model", default=os.path.join(wm.ROOT, "models", "world_cpu.osiewm"))
    args = ap.parse_args()
    binary = wm.lab_binary()
    out = {}
    sets = {}
    eps = [e for e in wm.load_episodes(os.path.join(args.data, "episodes.jsonl")) if e["valid"]]
    part = wm.split_runs(eps)
    for name in ["test", "ood_family"]:
        sets[name] = [e for e in eps if part[e["run_id"]] == name]
    sets["counterfactual"] = [e for e in wm.load_episodes(os.path.join(args.cf, "episodes.jsonl")) if e["valid"]]
    if args.batch:
        sets["batch_class"] = [e for e in wm.load_episodes(os.path.join(args.batch, "episodes.jsonl")) if e["valid"]]
    for name, es in sets.items():
        X, names, ok = wm.encode(es)
        Y = wm.targets(es)
        Ym = np.stack([Y[t] for t in ex.TARGETS], 1)
        for t in ex.TARGETS:
            ex.SCALE.setdefault(t, 1.0)
        lines = cpp(binary, args.model, es).splitlines()
        P = np.full((len(es), 4), np.nan)
        abst = np.zeros(len(es), bool)
        for line in lines:
            i, status, vals = line.split("\t", 2)
            i = int(i)
            if status == "ERROR":
                abst[i] = True
                continue
            abst[i] = status == "ABSTAIN"
            P[i] = [float(v) for v in vals.split(",")]
        mech = ex.mechanistic(X, names)
        acc = ~abst & np.all(np.isfinite(Ym), 1)
        runs = [e["run_id"] for e in es]
        res = {"episodes": len(es), "runs": len({e['run_id'] for e in es}), "abstained": int(abst.sum())}
        if name != "counterfactual":  # also report the validation-selected learned hybrid, if exported alongside
            pass
        for j, t in enumerate(ex.TARGETS):
            res[t] = {"deployed": wm.mae_with_ci(Ym[acc, j], P[acc, j], [runs[i] for i in np.where(acc)[0]]),
                      "mechanistic": wm.mae_with_ci(Ym[acc, j], mech[acc, j], [runs[i] for i in np.where(acc)[0]]),
                      "no_change": wm.mae_with_ci(Ym[acc, j], Y[t + "_pre"][acc], [runs[i] for i in np.where(acc)[0]])}
        out[name] = res
        print(name, f"abstained {res['abstained']}/{res['episodes']}",
              {t: (round(res[t]['deployed']['mae'], 3), round(res[t]['mechanistic']['mae'], 3)) for t in ex.TARGETS})
    bench = subprocess.run([binary, "bench", args.model], input=cpp_tsv(sets["test"]), capture_output=True, text=True,
                           check=True).stdout
    out["cpp_inference"] = json.loads(bench)
    print("C++ inference:", out["cpp_inference"])
    json.dump(out, open(os.path.join(args.results, "deployed_eval.json"), "w"), indent=1, default=float)


def cpp_tsv(eps):
    tsv = []
    for i, e in enumerate(eps):
        a = e["action"]
        wins = [",".join("nan" if w.get(f) is None else repr(float(w[f])) for f in wm.WINDOW_FIELDS) for w in e["pre"][-6:]]
        tsv.append(f"{i}\t{a['kind']} {a['previous']} {a['proposed']}\t{';'.join(wins)}")
    return "\n".join(tsv) + "\n"


if __name__ == "__main__":
    main()
