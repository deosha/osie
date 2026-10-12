#!/usr/bin/env python3
"""World-model experiments 1-8 (docs/WORLD_MODEL_EXPERIMENTS.md).

    python3 ml/world_model/experiments.py --data ml/world_model/data/v1 --out ml/world_model/results

All selection uses the validation runs. Test runs, the held-out workload
family and the held-out quotas are only used for reporting.
"""
import argparse
import json
import math
import os
import resource
import time
import warnings

import numpy as np
import torch
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.linear_model import Ridge

import wm

warnings.filterwarnings("ignore")
torch.set_num_threads(1)
TARGETS = list(wm.OBSERVABLE_TARGETS)
SEEDS = [1, 2, 3]


# ----------------------------------------------------------------- models ---

class Standardizer:
    def fit(self, X):
        self.mu = X.mean(0)
        self.sd = X.std(0)
        self.sd[self.sd < 1e-9] = 1.0
        return self

    def __call__(self, X):
        return (X - self.mu) / self.sd


def fit_ridge(Xtr, Ytr, Xva, Yva, seed=0):
    sx = Standardizer().fit(Xtr)
    best = None
    for alpha in [0.01, 0.1, 1, 10, 100]:  # chosen on validation
        m = Ridge(alpha=alpha).fit(sx(Xtr), Ytr)
        err = np.abs(m.predict(sx(Xva)) - Yva).mean()
        if best is None or err < best[0]:
            best = (err, m, alpha)
    m = best[1]
    pred = lambda X: m.predict(sx(X))
    params = m.coef_.size + m.intercept_.size
    return pred, {"params": int(params), "alpha": best[2], "model": m, "scaler": sx}


def fit_gbt(Xtr, Ytr, Xva, Yva, seed=0):
    models = []
    for j in range(Ytr.shape[1]):
        best = None
        for depth, lr in [(3, 0.05), (4, 0.1), (6, 0.1)]:
            m = HistGradientBoostingRegressor(max_depth=depth, learning_rate=lr, max_iter=300, random_state=seed,
                                              early_stopping=False).fit(Xtr, Ytr[:, j])
            err = np.abs(m.predict(Xva) - Yva[:, j]).mean()
            if best is None or err < best[0]:
                best = (err, m)
        models.append(best[1])
    pred = lambda X: np.stack([m.predict(X) for m in models], 1)
    nodes = sum(sum(len(p.nodes) for p in m._predictors[i]) for m in models for i in range(len(m._predictors)))
    return pred, {"params": int(nodes * 2), "note": "params = 2 x tree nodes (threshold + value)"}


class MLP(torch.nn.Module):
    def __init__(self, n, h, t, layers=1):
        super().__init__()
        mods, d = [], n
        for _ in range(layers):
            mods += [torch.nn.Linear(d, h), torch.nn.ReLU()]
            d = h
        mods.append(torch.nn.Linear(d, t))
        self.net = torch.nn.Sequential(*mods)

    def forward(self, x):
        return self.net(x)


def train_torch(model, Xtr, Ytr, Xva, Yva, seed, epochs=600, lr=3e-3, wd=1e-4):
    torch.manual_seed(seed)
    opt = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=wd)
    xt, yt = torch.tensor(Xtr, dtype=torch.float32), torch.tensor(Ytr, dtype=torch.float32)
    xv, yv = torch.tensor(Xva, dtype=torch.float32), torch.tensor(Yva, dtype=torch.float32)
    best, best_state, patience = math.inf, None, 0
    for ep in range(epochs):
        model.train()
        perm = torch.randperm(len(xt))
        for i in range(0, len(xt), 64):
            idx = perm[i:i + 64]
            opt.zero_grad()
            loss = torch.nn.functional.l1_loss(model(xt[idx]), yt[idx])
            loss.backward()
            opt.step()
        model.eval()
        with torch.no_grad():
            v = torch.nn.functional.l1_loss(model(xv), yv).item()
        if v < best - 1e-5:
            best, best_state, patience = v, {k: x.clone() for k, x in model.state_dict().items()}, 0
        else:
            patience += 1
            if patience > 60:
                break
    model.load_state_dict(best_state)
    return model


def fit_mlp(h, layers=1):
    def fit(Xtr, Ytr, Xva, Yva, seed=0):
        sx, sy = Standardizer().fit(Xtr), Standardizer().fit(Ytr)
        torch.manual_seed(seed)
        m = MLP(Xtr.shape[1], h, Ytr.shape[1], layers)
        m = train_torch(m, sx(Xtr), sy(Ytr), sx(Xva), sy(Yva), seed)
        m.eval()

        def pred(X):
            with torch.no_grad():
                return m(torch.tensor(sx(X), dtype=torch.float32)).numpy() * sy.sd + sy.mu
        return pred, {"params": int(sum(p.numel() for p in m.parameters())), "model": m, "sx": sx, "sy": sy, "hidden": h}
    return fit


class GRUNet(torch.nn.Module):
    def __init__(self, step_dim, static_dim, h, t):
        super().__init__()
        self.gru = torch.nn.GRU(step_dim, h, batch_first=True)
        self.head = torch.nn.Sequential(torch.nn.Linear(h + static_dim, h), torch.nn.ReLU(), torch.nn.Linear(h, t))

    def forward(self, seq, static):
        _, hn = self.gru(seq)
        return self.head(torch.cat([hn[-1], static], 1))


def window_seq(episodes):
    seq = []
    for e in episodes:
        steps = []
        for w in e["pre"][-6:]:
            row = []
            for f in wm.WINDOW_FIELDS:
                v = w.get(f)
                row += [0.0 if v is None else float(v), 1.0 if v is None else 0.0]
            steps.append(row)
        seq.append(steps)
    return np.array(seq)


def fit_gru(h):
    def fit(Str, Xtr, Ytr, Sva, Xva, Yva, seed=0):
        ss = Standardizer().fit(Str.reshape(-1, Str.shape[2]))
        sx, sy = Standardizer().fit(Xtr), Standardizer().fit(Ytr)
        torch.manual_seed(seed)
        m = GRUNet(Str.shape[2], Xtr.shape[1], h, Ytr.shape[1])
        opt = torch.optim.Adam(m.parameters(), lr=3e-3, weight_decay=1e-4)
        st = torch.tensor(ss(Str), dtype=torch.float32)
        xt, yt = torch.tensor(sx(Xtr), dtype=torch.float32), torch.tensor(sy(Ytr), dtype=torch.float32)
        sv, xv = torch.tensor(ss(Sva), dtype=torch.float32), torch.tensor(sx(Xva), dtype=torch.float32)
        yv = torch.tensor(sy(Yva), dtype=torch.float32)
        best, state, patience = math.inf, None, 0
        for ep in range(600):
            m.train()
            perm = torch.randperm(len(xt))
            for i in range(0, len(xt), 64):
                idx = perm[i:i + 64]
                opt.zero_grad()
                torch.nn.functional.l1_loss(m(st[idx], xt[idx]), yt[idx]).backward()
                opt.step()
            m.eval()
            with torch.no_grad():
                v = torch.nn.functional.l1_loss(m(sv, xv), yv).item()
            if v < best - 1e-5:
                best, state, patience = v, {k: x.clone() for k, x in m.state_dict().items()}, 0
            else:
                patience += 1
                if patience > 60:
                    break
        m.load_state_dict(state)
        m.eval()

        def pred(S, X):
            with torch.no_grad():
                return m(torch.tensor(ss(S), dtype=torch.float32), torch.tensor(sx(X), dtype=torch.float32)).numpy() * sy.sd + sy.mu
        return pred, {"params": int(sum(p.numel() for p in m.parameters()))}
    return fit


# ------------------------------------------------------------- baselines ---

class ResidualPred:
    """Callable on the selected feature columns; recovers the mechanistic prior
    from those columns (they always include the m_/g_act features it needs)."""

    def __init__(self, rp, cols, names):
        self.rp, self.cols, self.names = rp, cols, names

    def __call__(self, Z):
        full = np.zeros((Z.shape[0], len(self.names)))
        full[:, self.cols] = Z
        return mechanistic(full, self.names) + self.rp(Z)


def no_change(Y, idx):
    return np.stack([Y[t + "_pre"][idx] for t in TARGETS], 1)


def mechanistic(X, names):
    """Documented cgroup v2 CPU bandwidth semantics only (no fitting)."""
    c = {n: X[:, i] for i, n in enumerate(names)}
    quota_after = np.where(c["g_act_quota"] > 0, c["g_act_proposed"],
                           np.where(c["g_quota_unlimited"] > 0, np.inf, c["g_quota"]))
    cpus_after = np.where(c["g_act_cpuset"] > 0, c["g_act_proposed"], c["g_cpuset"])
    demand = c["m_demand_after"]
    binds = quota_after < np.minimum(demand, cpus_after)
    usage = np.minimum(demand, np.minimum(quota_after, cpus_after))
    throttled = np.where(binds, demand - np.minimum(quota_after, cpus_after), 0.0)
    ratio = np.where(binds, 1.0, 0.0)
    psi = c["g_psi_some"]  # no documented closed form: unchanged
    return np.stack([usage, throttled, ratio, psi], 1)


# ------------------------------------------------------------ evaluation ---

def evaluate(Y, P, idx, runs, eps, X=None, names=None):
    out = {}
    for j, t in enumerate(TARGETS):
        y = Y[t][idx]
        out[t] = wm.mae_with_ci(y, P[:, j], [runs[i] for i in idx])
    out["mean_norm_mae"] = float(np.mean([out[t]["mae"] / SCALE[t] for t in TARGETS]))
    # Direction of effect on interventions with a material true change.
    dirs = {}
    for j, t in enumerate(["usage_cores", "throttled_cores"]):
        jj = TARGETS.index(t)
        d_true = Y[t][idx] - Y[t + "_pre"][idx]
        d_pred = P[:, jj] - Y[t + "_pre"][idx]
        mask = (np.abs(d_true) > 0.1) & np.array([eps[i]["action"]["kind"] != "none" for i in idx])
        if mask.sum():
            dirs[t] = {"accuracy": float((np.sign(d_true[mask]) == np.sign(d_pred[mask])).mean()), "n": int(mask.sum())}
    out["direction"] = dirs
    # Physical plausibility of raw outputs (H3): usage outside [0, capacity], negative quantities.
    if X is not None:
        cap = X[:, names.index("m_cap_after")]
        def viol(tol):
            return ((P[:, 0] < -tol) | (P[:, 0] > cap + tol) | (P[:, 1] < -tol) | (P[:, 2] < -tol) | (P[:, 2] > 1 + tol)
                    | (P[:, 3] < -tol) | (P[:, 3] > 1 + tol))
        out["implausible_rate"] = float(viol(1e-6).mean())          # any violation, however small
        out["implausible_rate_material"] = float(viol(0.05).mean())  # violation > 0.05 cores / 5 pp
    return out


SCALE = {}


def norm_err(Y, P, idx):
    """Per-episode error, averaged over targets after normalising each target."""
    return np.mean([np.abs(Y[t][idx] - P[:, j]) / SCALE[t] for j, t in enumerate(TARGETS)], 0)


def paired_ci(err_model, err_base, run_ids, n=2000, seed=0):
    """Run-bootstrap 95% CI of mean(err_model - err_base). Negative = model better."""
    d = err_model - err_base
    by = {}
    for r, v in zip(run_ids, d):
        by.setdefault(r, []).append(v)
    by = {k: np.array(v) for k, v in by.items()}
    lo, hi = wm.bootstrap_ci(by, n=n, seed=seed)
    return {"mean_diff": float(d.mean()), "ci95": [lo, hi]}
BASE_FITS = {"ridge": fit_ridge, "gbt": fit_gbt, "mlp_h16": fit_mlp(16)}


def fit_any(mname, Xs, Ymat, tr, va, cols, names, seed):
    """Fits `mname` (plain or res_* hybrid) on rows tr; returns pred(Z_cols)."""
    if mname.startswith("res_"):
        mech = mechanistic(Xs, names)
        rp, _ = BASE_FITS[mname[4:]](Xs[tr][:, cols], Ymat[tr] - mech[tr], Xs[va][:, cols], Ymat[va] - mech[va], seed=seed)
        return ResidualPred(rp, cols, names)
    pred, _ = BASE_FITS[mname](Xs[tr][:, cols], Ymat[tr], Xs[va][:, cols], Ymat[va], seed=seed)
    return pred


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    t_start = time.time()

    eps_all = wm.load_episodes(os.path.join(args.data, "episodes.jsonl"))
    eps = [e for e in eps_all if e["valid"]]
    X, names, ok = wm.encode(eps)
    eps = [e for e, k in zip(eps, ok) if k]
    X = X[ok]
    Y = wm.targets(eps)
    keep = np.all([np.isfinite(Y[t]) & np.isfinite(Y[t + "_pre"]) for t in TARGETS], axis=0)
    eps, X = [e for e, k in zip(eps, keep) if k], X[keep]
    Y = {k: v[keep] for k, v in Y.items()}
    Ymat = np.stack([Y[t] for t in TARGETS], 1)
    runs = [e["run_id"] for e in eps]
    part = wm.split_runs(eps)
    P = np.array([part[r] for r in runs])
    idx = {p: np.where(P == p)[0] for p in ["train", "val", "test", "ood_family"]}
    qa = np.array([e["action"]["proposed"] if e["action"]["kind"] == "cpu_quota" else math.nan for e in eps])
    heldq = np.isin(qa, wm.HELDOUT_QUOTAS)
    for t in TARGETS:
        SCALE[t] = float(np.std(Y[t][idx["train"]])) or 1.0

    summary = {
        "episodes_total": len(eps_all), "episodes_valid": len(eps),
        "runs": {p: int(len({runs[i] for i in ix})) for p, ix in idx.items()},
        "episodes": {p: int(len(ix)) for p, ix in idx.items()},
        "actions": {k: int(sum(e["action"]["kind"] == k for e in eps)) for k in ["none", "cpu_quota", "cpuset", "offered_load"]},
        "target_scale_train_std": SCALE,
    }
    print(json.dumps(summary, indent=1))
    res = {"summary": summary}

    sets = {k: wm.group_cols(names, v) for k, v in wm.FEATURE_SETS.items()}
    tr, va = idx["train"], idx["val"]
    eval_sets = {"val": idx["val"], "test": idx["test"], "ood_family": idx["ood_family"]}

    # ---- Experiments 1 + 2: feature sets x model families -----------------
    print("\n== Experiments 1-2: features x models")
    candidates = {"ridge": fit_ridge, "gbt": fit_gbt, "mlp_h16": fit_mlp(16), "mlp_h128": fit_mlp(128),
                  "mlp_2x384": fit_mlp(384, 2),
                  # architecture-constrained hybrids: mechanistic prior + learned residual
                  "res_ridge": "residual:ridge", "res_mlp_h16": "residual:mlp_h16"}
    MECH_ALL = mechanistic(X, names)
    exp12 = {"no_change": {}, "mechanistic": {}}
    for name, ix in eval_sets.items():
        exp12["no_change"][name] = evaluate(Y, no_change(Y, ix), ix, runs, eps, X[ix], names)
        exp12["mechanistic"][name] = evaluate(Y, mechanistic(X[ix], names), ix, runs, eps, X[ix], names)
    trained = {}
    for fs, cols in sets.items():
        for mname, fit in candidates.items():
            if isinstance(fit, str) and fs != "both":
                continue  # the prior needs both raw quota/cpuset values and mechanism features
            key = f"{mname}/{fs}"
            per_seed = []
            seeds = SEEDS if "mlp" in mname else [0]
            for s in seeds:
                t0 = time.time()
                r0 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
                if isinstance(fit, str):
                    base_fit = {"ridge": fit_ridge, "mlp_h16": fit_mlp(16)}[fit.split(":")[1]]
                    rp, info = base_fit(X[tr][:, cols], Ymat[tr] - MECH_ALL[tr], X[va][:, cols], Ymat[va] - MECH_ALL[va], seed=s)
                    pred = ResidualPred(rp, cols, names)
                    info["residual"] = True
                else:
                    pred, info = fit(X[tr][:, cols], Ymat[tr], X[va][:, cols], Ymat[va], seed=s)
                train_s = time.time() - t0
                t1 = time.perf_counter()
                for _ in range(20):
                    pred(X[idx["test"]][:1, cols])
                infer_us = (time.perf_counter() - t1) / 20 * 1e6
                ev = {name: evaluate(Y, pred(X[ix][:, cols]), ix, runs, eps, X[ix], names) for name, ix in eval_sets.items()}
                for name in ["test", "ood_family"]:
                    ix = eval_sets[name]
                    ev[name]["vs_mechanistic"] = paired_ci(norm_err(Y, pred(X[ix][:, cols]), ix),
                                                           norm_err(Y, mechanistic(X[ix], names), ix), [runs[i] for i in ix])
                per_seed.append({"eval": ev, "params": info["params"], "train_s": train_s, "infer_us_python": infer_us,
                                 "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1 << 20)})
                trained[(key, s)] = (pred, info, cols)
            agg = {"params": per_seed[0]["params"], "train_s": float(np.mean([p["train_s"] for p in per_seed])),
                   "infer_us_python": float(np.mean([p["infer_us_python"] for p in per_seed])), "seeds": len(per_seed)}
            for name in eval_sets:
                vals = [p["eval"][name]["mean_norm_mae"] for p in per_seed]
                agg[name] = {"mean_norm_mae": float(np.mean(vals)), "std": float(np.std(vals)),
                             "per_target": per_seed[0]["eval"][name]}
                if "vs_mechanistic" in per_seed[0]["eval"][name]:
                    agg[name]["vs_mechanistic_seed1"] = per_seed[0]["eval"][name]["vs_mechanistic"]
            exp12[key] = agg
            vt, vo = agg["test"]["vs_mechanistic_seed1"], agg["ood_family"]["vs_mechanistic_seed1"]
            print(f"{key:22s} params={agg['params']:>7} val={agg['val']['mean_norm_mae']:.3f} "
                  f"test={agg['test']['mean_norm_mae']:.3f}±{agg['test']['std']:.3f} ood={agg['ood_family']['mean_norm_mae']:.3f} "
                  f"| vs mech test {vt['mean_diff']:+.3f} [{vt['ci95'][0]:+.3f},{vt['ci95'][1]:+.3f}] "
                  f"ood {vo['mean_diff']:+.3f} [{vo['ci95'][0]:+.3f},{vo['ci95'][1]:+.3f}]")
    for b in ["no_change", "mechanistic"]:
        print(f"{b:22s} val={exp12[b]['val']['mean_norm_mae']:.3f} test={exp12[b]['test']['mean_norm_mae']:.3f} ood={exp12[b]['ood_family']['mean_norm_mae']:.3f}")
    res["exp1_2_features_models"] = exp12

    # ---- Experiment 4: history length and a temporal model ---------------
    print("\n== Experiment 4: history")
    exp4 = {}
    for hlen in [1, 2, 4, 6]:
        Xh, _, okh = wm.encode(eps, history=hlen)
        cols = sets["both"]
        for mname in ["ridge", "gbt", "mlp_h16", "res_ridge", "res_mlp_h16"]:
            vals = []
            for s in (SEEDS if "mlp" in mname else [0]):
                pred = fit_any(mname, Xh, Ymat, tr, va, cols, names, s)
                vals.append(evaluate(Y, pred(Xh[idx["test"]][:, cols]), idx["test"], runs, eps)["mean_norm_mae"])
            exp4[f"{mname}/history{hlen}"] = {"test_mean_norm_mae": float(np.mean(vals)), "std": float(np.std(vals))}
            print(f"{mname}/history{hlen}: {np.mean(vals):.3f}")
    S = window_seq(eps)
    X1, _, _ = wm.encode(eps, history=1)
    static_cols = [i for i, n in enumerate(names) if n.startswith(("g_act", "m_cap", "m_log_cap"))]
    for h in [16, 32]:
        vals, params = [], 0
        for s in SEEDS:
            pred, info = fit_gru(h)(S[tr], X1[tr][:, static_cols], Ymat[tr], S[va], X1[va][:, static_cols], Ymat[va], seed=s)
            vals.append(evaluate(Y, pred(S[idx["test"]], X1[idx["test"]][:, static_cols]), idx["test"], runs, eps)["mean_norm_mae"])
            params = info["params"]
        exp4[f"gru_h{h}/raw_windows"] = {"test_mean_norm_mae": float(np.mean(vals)), "std": float(np.std(vals)), "params": params}
        print(f"gru_h{h} (raw windows + action): {np.mean(vals):.3f} params={params}")
    res["exp4_history"] = exp4

    # ---- Experiment 3: data efficiency (learning curves) ------------------
    print("\n== Experiment 3: learning curves")
    exp3 = {}
    train_runs = sorted({runs[i] for i in tr})
    by_fam = {f: [r for r in train_runs if r.startswith(f + "-")] for f in wm.TRAIN_FAMILIES}
    for k in [1, 2, 3, 4, 6]:
        for fs in ["generic", "mechanism", "both"]:
            for mname in ["ridge", "gbt", "mlp_h16"] + (["res_ridge", "res_mlp_h16"] if fs == "both" else []):
                vals = []
                for s in SEEDS:
                    rng = np.random.RandomState(100 + s)
                    chosen = set()
                    for f, rs in by_fam.items():
                        chosen |= set(rng.choice(rs, min(k, len(rs)), replace=False))
                    sub = np.array([i for i in tr if runs[i] in chosen])
                    pred = fit_any(mname, X, Ymat, sub, va, sets[fs], names, s)
                    vals.append(evaluate(Y, pred(X[idx["test"]][:, sets[fs]]), idx["test"], runs, eps)["mean_norm_mae"])
                exp3[f"{mname}/{fs}/runs_per_family={k}"] = {"test_mean_norm_mae": float(np.mean(vals)),
                                                              "std": float(np.std(vals)),
                                                              "train_episodes": int(len(sub))}
                print(f"k={k} {mname}/{fs}: {np.mean(vals):.3f}±{np.std(vals):.3f} ({len(sub)} episodes)")
    res["exp3_learning_curves"] = exp3

    # ---- Experiment 5: unseen quota values ---------------------------------
    print("\n== Experiment 5: unseen quotas")
    tr5 = np.array([i for i in tr if not heldq[i]])
    va5 = np.array([i for i in va if not heldq[i]])
    te5 = np.array([i for i in idx["test"] if heldq[i]])
    exp5 = {"test_episodes": int(len(te5)), "train_episodes": int(len(tr5)),
            "no_change": evaluate(Y, no_change(Y, te5), te5, runs, eps)["mean_norm_mae"],
            "mechanistic": evaluate(Y, mechanistic(X[te5], names), te5, runs, eps)["mean_norm_mae"]}
    for fs in ["generic", "mechanism", "both"]:
        for mname in ["ridge", "gbt", "mlp_h16"] + (["res_ridge", "res_mlp_h16"] if fs == "both" else []):
            vals = []
            for s in (SEEDS if "mlp" in mname else [0]):
                pred = fit_any(mname, X, Ymat, tr5, va5, sets[fs], names, s)
                vals.append(evaluate(Y, pred(X[te5][:, sets[fs]]), te5, runs, eps)["mean_norm_mae"])
            exp5[f"{mname}/{fs}"] = float(np.mean(vals))
    print(json.dumps(exp5, indent=1))
    res["exp5_unseen_quotas"] = exp5

    # ---- Selection on validation, then calibration + OOD -------------------
    print("\n== Selection (validation only)")
    ranking = sorted(((v["val"]["mean_norm_mae"], k) for k, v in exp12.items() if "/" in k))
    exportable = [k for _, k in ranking if k.split("/")[0] in ("ridge", "mlp_h16", "mlp_h128", "res_ridge", "res_mlp_h16")]
    best_any, best_export = ranking[0][1], exportable[0]
    print("best on validation:", best_any, "| best exportable to C++:", best_export)
    res["selection"] = {"ranking_val": ranking[:10], "best_any": best_any, "best_exportable": best_export}

    # Split-conformal half-widths from validation residuals of the exported model.
    pred, info, cols = trained[(best_export, 1 if "mlp" in best_export else 0)]
    Pva = pred(X[va][:, cols])
    conformal = {}
    for j, t in enumerate(TARGETS):
        r = np.abs(Ymat[va][:, j] - Pva[:, j])
        n = len(r)
        conformal[t] = {str(lvl): float(np.quantile(r, min(1.0, math.ceil((n + 1) * lvl) / n))) for lvl in [0.8, 0.95]}
    cov = {}
    for name in ["test", "ood_family"]:
        ix = idx[name]
        Pt = pred(X[ix][:, cols])
        cov[name] = {t: {lvl: float((np.abs(Ymat[ix][:, j] - Pt[:, j]) <= conformal[t][lvl]).mean())
                         for lvl in conformal[t]} for j, t in enumerate(TARGETS)}
    # OOD detector: training feature ranges (+5% margin) and mean squared z-score
    sx = wm_std = Standardizer().fit(X[tr][:, cols])
    lo, hi = X[tr][:, cols].min(0), X[tr][:, cols].max(0)
    margin = 0.05 * (hi - lo) + 1e-6
    score = lambda Z: (sx(Z) ** 2).mean(1)
    thr = float(np.quantile(score(X[va][:, cols]), 0.99))

    def flagged(ix):
        Z = X[ix][:, cols]
        return ((Z < lo - margin) | (Z > hi + margin)).any(1) | (score(Z) > thr)
    ood = {name: float(flagged(idx[name]).mean()) for name in ["val", "test", "ood_family"]}
    # Error on accepted vs abstained predictions
    acc = {}
    for name in ["test", "ood_family"]:
        ix = idx[name]
        f = flagged(ix)
        Pt = pred(X[ix][:, cols])
        e = np.mean([np.abs(Ymat[ix][:, j] - Pt[:, j]) / SCALE[t] for j, t in enumerate(TARGETS)], 0)
        acc[name] = {"flagged_rate": float(f.mean()), "norm_mae_accepted": float(e[~f].mean()) if (~f).any() else None,
                     "norm_mae_flagged": float(e[f].mean()) if f.any() else None}
    res["calibration"] = {"conformal_half_widths": conformal, "coverage": cov, "ood_flag_rate": ood,
                          "ood_threshold": thr, "accept_vs_abstain": acc}
    print(json.dumps(res["calibration"], indent=1))

    # False-safe rate: a quota change predicted to leave throttling low
    # (point estimate <= 0.1 cores, and separately the 80% upper bound <= 0.25)
    # while the observed throttling exceeded 0.5 cores.
    fs_out = {}
    for name in ["test", "ood_family"]:
        ix = np.array([i for i in idx[name] if eps[i]["action"]["kind"] == "cpu_quota"])
        Pt = pred(X[ix][:, cols])
        bad = Ymat[ix][:, 1] > 0.5
        for rule, safe in [("point<=0.1", Pt[:, 1] <= 0.1),
                           ("upper80<=0.25", Pt[:, 1] + conformal["throttled_cores"]["0.8"] <= 0.25)]:
            fs_out[f"{name}/{rule}"] = {"quota_changes": int(len(ix)), "predicted_safe": int(safe.sum()),
                                        "false_safe": int((safe & bad).sum()),
                                        "false_safe_rate": float((safe & bad).sum() / max(1, safe.sum()))}
        mb = mechanistic(X[ix], names)[:, 1] <= 0.1
        fs_out[f"{name}/mechanistic point<=0.1"] = {"predicted_safe": int(mb.sum()), "false_safe": int((mb & bad).sum()),
                                                    "false_safe_rate": float((mb & bad).sum() / max(1, mb.sum()))}
    res["false_safe"] = fs_out
    print("false-safe:", fs_out)

    res["export"] = {"model": best_export, "features": [names[c] for c in cols], "conformal": conformal,
                     "ood": {"lo": (lo - margin).tolist(), "hi": (hi + margin).tolist(), "score_max": thr}}
    res["wall_time_s"] = time.time() - t_start
    with open(os.path.join(args.out, "experiments.json"), "w") as f:
        json.dump(res, f, indent=1, default=float)
    print(f"\nwrote {args.out}/experiments.json in {res['wall_time_s']:.0f}s")


if __name__ == "__main__":
    main()
