#!/usr/bin/env python3
"""v0.2 world-model development: selection by grouped cross-validation on the
development data only (docs/WORLD_MODEL_EXPERIMENTS.md, "v0.2 protocol").

    python3 v02_dev.py --out results/v02 [--quick]

Partitions (run level, seeded, stratified by workload family):
  fit    ~70% of the development runs: model fitting and *all* selection,
         by leave-one-family-out (LOFO) and grouped-run cross-validation
  calib  ~30%: touched only by v02_freeze.py, for conformal calibration and
         the OOD threshold of the frozen model
  test   none here. The confirmatory test data are collected after the model
         is frozen (v02_confirm.py).

Nothing in this script reads the calibration runs except to count them.
"""
import argparse
import itertools
import json
import math
import os
import random
import time
import warnings

import numpy as np
import torch
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.linear_model import LogisticRegression, Ridge

import wm

warnings.filterwarnings("ignore")
torch.set_num_threads(2)
TARGETS = ["usage_cores", "throttled_cores", "throttled_ratio", "psi_some"]
DEV_SETS = ["data/v1", "data/cf1", "data/batch1"]
PARTITION_SEED = 20261013


# ------------------------------------------------------------------ data ---

def family_of(e):
    f = e["family"]
    return "batch" if f.startswith("batch") else f


def load_dev(paths=DEV_SETS):
    eps = []
    for d in paths:
        for e in wm.load_episodes(os.path.join(d, "episodes.jsonl")):
            if e["valid"]:
                e = dict(e, dataset=os.path.basename(d))
                eps.append(e)
    X, names, ok = wm.encode(eps)
    eps, X = [e for e, k in zip(eps, ok) if k], X[ok]
    Y = wm.targets(eps)
    keep = np.all([np.isfinite(Y[t]) & np.isfinite(Y[t + "_pre"]) for t in TARGETS], axis=0)
    eps, X = [e for e, k in zip(eps, keep) if k], X[keep]
    Y = {k: v[keep] for k, v in Y.items()}
    return eps, X, names, Y


def partition(eps, seed=PARTITION_SEED, calib_share=0.3):
    """run -> 'fit' | 'calib', stratified by family."""
    rng = random.Random(seed)
    by = {}
    for e in eps:
        by.setdefault(family_of(e), set()).add(e["run_id"])
    part = {}
    for fam in sorted(by):
        runs = sorted(by[fam])
        rng.shuffle(runs)
        k = max(1, round(len(runs) * calib_share))
        for r in runs[:k]:
            part[r] = "calib"
        for r in runs[k:]:
            part[r] = "fit"
    return part


def pre_mean(e, key):
    return wm.mean_field(e["pre"], key)


# ------------------------------------------------------------- mechanism ---

def mechanistic(X, names, demand_override=None):
    """cgroup v2 bandwidth semantics (no fitting). `demand_override` replaces
    the encoder's demand estimate (usage + throttled time) when given."""
    c = {n: X[:, i] for i, n in enumerate(names)}
    quota_after = np.where(c["g_act_quota"] > 0, c["g_act_proposed"],
                           np.where(c["g_quota_unlimited"] > 0, np.inf, c["g_quota"]))
    cpus_after = np.where(c["g_act_cpuset"] > 0, c["g_act_proposed"], c["g_cpuset"])
    demand = c["m_demand_after"] if demand_override is None else np.minimum(demand_override, cpus_after)
    cap = np.minimum(quota_after, cpus_after)
    binds = quota_after < np.minimum(demand, cpus_after)
    usage = np.minimum(demand, cap)
    throttled = np.where(binds, demand - cap, 0.0)
    ratio = np.where(binds, 1.0, 0.0)
    psi = c["g_psi_some"]
    return np.stack([usage, throttled, ratio, psi], 1)


def demand_psi(eps, X, names):
    """E2: usage + cgroup PSI some, scaled by an offered-load intervention."""
    c = {n: X[:, i] for i, n in enumerate(names)}
    u = np.array([pre_mean(e, "usage_cores") for e in eps])
    p = np.nan_to_num(np.array([pre_mean(e, "psi_some") for e in eps]))
    lf = np.where(c["g_act_load"] > 0, c["g_act_proposed"], 1.0)
    return (u + p) * lf


# ---------------------------------------------------------------- models ---

class Std:
    def fit(self, X):
        self.mu, self.sd = X.mean(0), X.std(0)
        self.sd[self.sd < 1e-9] = 1.0
        return self

    def __call__(self, X):
        return (X - self.mu) / self.sd


def loss_fn(kind, asym=1.0):
    def f(p, y):
        d = p - y
        if kind == "mae":
            l = d.abs()
        elif kind == "huber":
            l = torch.nn.functional.huber_loss(p, y, reduction="none", delta=0.5)
        elif kind == "quantile":  # pinball at the median
            l = torch.maximum(0.5 * (-d), -0.5 * (-d))
        else:
            raise ValueError(kind)
        if asym != 1.0:  # under-prediction (d < 0) costs `asym` times more
            l = torch.where(d < 0, asym * l, l)
        return l.mean()
    return f


def fit_mlp(Xtr, Ytr, hidden, lr, wd, batch, loss, seed, epochs=400, Xva=None, Yva=None, asym=None):
    torch.manual_seed(seed)
    np.random.seed(seed)
    sx, sy = Std().fit(Xtr), Std().fit(Ytr)
    xt = torch.tensor(sx(Xtr), dtype=torch.float32)
    yt = torch.tensor(sy(Ytr), dtype=torch.float32)
    m = torch.nn.Sequential(torch.nn.Linear(Xtr.shape[1], hidden), torch.nn.ReLU(), torch.nn.Linear(hidden, Ytr.shape[1]))
    opt = torch.optim.Adam(m.parameters(), lr=lr, weight_decay=wd)
    asym = asym or [1.0] * Ytr.shape[1]
    fns = [loss_fn(loss, a) for a in asym]
    have_va = Xva is not None and len(Xva) > 0
    if have_va:
        xv = torch.tensor(sx(Xva), dtype=torch.float32)
        yv = torch.tensor(sy(Yva), dtype=torch.float32)
    best, state, patience = math.inf, None, 0
    n = len(xt)
    for ep in range(epochs):
        m.train()
        perm = torch.randperm(n)
        for i in range(0, n, batch):
            idx = perm[i:i + batch]
            opt.zero_grad()
            out = m(xt[idx])
            l = sum(fns[j](out[:, j], yt[idx, j]) for j in range(Ytr.shape[1]))
            l.backward()
            opt.step()
        if have_va:
            m.eval()
            with torch.no_grad():
                o = m(xv)
                v = sum(fns[j](o[:, j], yv[:, j]).item() for j in range(Ytr.shape[1]))
            if v < best - 1e-5:
                best, state, patience = v, {k: x.clone() for k, x in m.state_dict().items()}, 0
            else:
                patience += 1
                if patience > 40:
                    break
    if state is not None:
        m.load_state_dict(state)
    m.eval()

    def pred(X):
        with torch.no_grad():
            return m(torch.tensor(sx(X), dtype=torch.float32)).numpy() * sy.sd + sy.mu
    pred.params = sum(p.numel() for p in m.parameters())
    pred.torch = (m, sx, sy)
    return pred


def fit_ridge(Xtr, Ytr, alpha):
    sx = Std().fit(Xtr)
    r = Ridge(alpha=alpha).fit(sx(Xtr), Ytr)
    pred = lambda X: r.predict(sx(X)).reshape(len(X), -1)
    pred.params = r.coef_.size + np.size(r.intercept_)
    pred.ridge = (r, sx)
    return pred


def fit_gbt(Xtr, Ytr, depth, lr, seed):
    ms = [HistGradientBoostingRegressor(max_depth=depth, learning_rate=lr, max_iter=200, random_state=seed,
                                        early_stopping=False).fit(Xtr, Ytr[:, j]) for j in range(Ytr.shape[1])]
    pred = lambda X: np.stack([m.predict(X) for m in ms], 1)
    pred.params = -1
    return pred


def inner_split(tr, runs, seed):
    """Early-stopping split inside a training fold, by run."""
    rs = sorted({runs[i] for i in tr})
    rng = random.Random(seed)
    rng.shuffle(rs)
    va_runs = set(rs[: max(1, len(rs) // 6)])
    va = np.array([i for i in tr if runs[i] in va_runs])
    fi = np.array([i for i in tr if runs[i] not in va_runs])
    return fi, va


# ------------------------------------------------------------- evaluation ---

def norm_mae(Y, P, ix, scale, targets=TARGETS):
    return {t: float(np.abs(Y[ix, j] - P[:, j]).mean() / scale[t]) for j, t in enumerate(targets)}


def folds_lofo(eps, idx):
    fams = sorted({family_of(eps[i]) for i in idx})
    return [(f, np.array([i for i in idx if family_of(eps[i]) != f]), np.array([i for i in idx if family_of(eps[i]) == f]))
            for f in fams]


def folds_runs(eps, idx, k=5, seed=1):
    runs = sorted({eps[i]["run_id"] for i in idx})
    rng = random.Random(seed)
    rng.shuffle(runs)
    out = []
    for j in range(k):
        held = set(runs[j::k])
        out.append((f"runs{j}", np.array([i for i in idx if eps[i]["run_id"] not in held]),
                    np.array([i for i in idx if eps[i]["run_id"] in held])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results/v02")
    ap.add_argument("--quick", action="store_true", help="smaller search (for smoke tests)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    eps, X, names, Yd = load_dev()
    Y = np.stack([Yd[t] for t in TARGETS], 1)
    runs = [e["run_id"] for e in eps]
    part = partition(eps)
    fit_idx = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "fit"])
    cal_idx = np.array([i for i, e in enumerate(eps) if part[e["run_id"]] == "calib"])
    scale = {t: float(np.std(Y[fit_idx, j])) or 1.0 for j, t in enumerate(TARGETS)}
    fams = sorted({family_of(e) for e in eps})
    summary = {"episodes": len(eps), "runs": len(set(runs)), "families": fams,
               "fit": {"runs": len({runs[i] for i in fit_idx}), "episodes": int(len(fit_idx))},
               "calib": {"runs": len({runs[i] for i in cal_idx}), "episodes": int(len(cal_idx))},
               "runs_by_family_and_partition": {f: {p: sorted({r for r, q in part.items() if q == p and
                                                               any(family_of(e) == f for e in eps if e["run_id"] == r)})
                                                    for p in ("fit", "calib")} for f in fams},
               "target_scale": scale, "partition_seed": PARTITION_SEED}
    print(json.dumps({k: v for k, v in summary.items() if k != "runs_by_family_and_partition"}, indent=1))
    res = {"summary": summary}

    mech_e1 = mechanistic(X, names)
    mech_e2 = mechanistic(X, names, demand_psi(eps, X, names))
    cols = list(range(len(names)))

    lofo = folds_lofo(eps, fit_idx)
    rfold = folds_runs(eps, fit_idx)

    def cv(fit_predict, folds):
        """fit_predict(tr, te) -> P[te]; returns per-fold per-target normalised MAE."""
        out = {}
        for name, tr, te in folds:
            P = fit_predict(tr, te)
            out[name] = norm_mae(Y, P, te, scale)
        return out

    def agg(cvres):
        per_t = {t: float(np.mean([v[t] for v in cvres.values()])) for t in TARGETS}
        worst = {t: float(np.max([v[t] for v in cvres.values()])) for t in TARGETS}
        return {"mean": per_t, "worst_fold": worst,
                "core": float(np.mean([per_t["usage_cores"], per_t["throttled_cores"]])),
                "core_worst": float(np.max([np.mean([v["usage_cores"], v["throttled_cores"]]) for v in cvres.values()])),
                "folds": cvres}

    # ---- 1. demand estimator ablation (zero-parameter) -------------------
    print("\n== mechanistic demand estimators (LOFO and run-CV, fit partition)")
    exp = {}
    for nm, M in [("mech_E1_usage+throttled", mech_e1), ("mech_E2_usage+psi", mech_e2),
                  ("no_change", np.stack([Yd[t + "_pre"] for t in TARGETS], 1))]:
        exp[nm] = {"lofo": agg(cv(lambda tr, te, M=M: M[te], lofo)), "runs": agg(cv(lambda tr, te, M=M: M[te], rfold))}
        print(f"{nm:28s} LOFO core={exp[nm]['lofo']['core']:.3f} (worst {exp[nm]['lofo']['core_worst']:.3f}) "
              f"run-CV core={exp[nm]['runs']['core']:.3f} | " +
              " ".join(f"{t[:5]}={exp[nm]['lofo']['mean'][t]:.3f}" for t in TARGETS))
    res["mechanistic"] = exp

    # ---- 2. model sweep (bounded random search) ---------------------------
    print("\n== model sweep (LOFO on fit partition; selection metric = LOFO core, ties -> fewer params)")
    rng = random.Random(7)
    grid = list(itertools.product([3e-4, 1e-3, 3e-3], [0.0, 1e-4, 1e-3], [16, 32, 64], ["mae", "huber", "quantile"]))
    n_cfg = 3 if a.quick else 10
    sweep = {}

    def run_model(kind, hidden=None, cfg=None, residual_on=None, seeds=(1, 2)):
        prior = {"e1": mech_e1, "e2": mech_e2, None: np.zeros_like(Y)}[residual_on]

        def fp(tr, te, seed):
            fi, va = inner_split(tr, runs, seed)
            T = Y - prior
            if kind == "mlp":
                lr, wd, bs, loss = cfg
                p = fit_mlp(X[fi], T[fi], hidden, lr, wd, bs, loss, seed, Xva=X[va], Yva=T[va])
            elif kind == "ridge":
                p = fit_ridge(X[tr], T[tr], cfg)
            elif kind == "gbt":
                p = fit_gbt(X[tr], T[tr], cfg[0], cfg[1], seed)
            P = p(X[te]) + prior[te]
            fp.params = p.params
            return P
        per_seed = [agg(cv(lambda tr, te, s=s: fp(tr, te, s), lofo)) for s in seeds]
        r = {"core": float(np.mean([p["core"] for p in per_seed])), "core_sd": float(np.std([p["core"] for p in per_seed])),
             "core_worst": float(np.mean([p["core_worst"] for p in per_seed])),
             "mean": {t: float(np.mean([p["mean"][t] for p in per_seed])) for t in TARGETS},
             "seeds": len(seeds), "params": int(getattr(fp, "params", -1))}
        return r

    for residual_on in [None, "e1", "e2"]:
        tag = {None: "direct", "e1": "res_e1", "e2": "res_e2"}[residual_on]
        for alpha in [0.1, 1.0, 10.0, 100.0]:
            k = f"ridge_a{alpha:g}/{tag}"
            sweep[k] = run_model("ridge", cfg=alpha, residual_on=residual_on, seeds=(0,))
        for depth, lr in [(3, 0.05), (4, 0.1)]:
            k = f"gbt_d{depth}_lr{lr:g}/{tag}"
            sweep[k] = run_model("gbt", cfg=(depth, lr), residual_on=residual_on, seeds=(0,))
        for hidden in [8, 16, 32, 64]:
            for cfg in rng.sample(grid, n_cfg):
                k = f"mlp_h{hidden}_lr{cfg[0]:g}_wd{cfg[1]:g}_b{cfg[2]}_{cfg[3]}/{tag}"
                sweep[k] = run_model("mlp", hidden, cfg, residual_on)
        best = sorted((v["core"], k) for k, v in sweep.items() if k.endswith("/" + tag))[:5]
        for c, k in best:
            print(f"  {k:52s} LOFO core={c:.3f}±{sweep[k]['core_sd']:.3f} worst={sweep[k]['core_worst']:.3f} params={sweep[k]['params']}")
    res["sweep"] = sweep

    # Top configuration per family of model, re-run with 5 seeds for variance.
    top = {}
    for tag in ["direct", "res_e1", "res_e2"]:
        mlps = sorted((v["core"], k) for k, v in sweep.items() if k.endswith("/" + tag) and k.startswith("mlp"))
        if mlps:
            k = mlps[0][1]
            h = int(k.split("_")[1][1:])
            parts = k.split("/")[0].split("_")
            cfg = (float(parts[2][2:]), float(parts[3][2:]), int(parts[4][1:]), parts[5])
            top[k] = run_model("mlp", h, cfg, {"direct": None, "res_e1": "e1", "res_e2": "e2"}[tag], seeds=(1, 2, 3, 4, 5))
            print(f"5 seeds {k}: core={top[k]['core']:.3f}±{top[k]['core_sd']:.3f}")
    res["top_5seed"] = top

    # ---- 3. target-specific models and losses ------------------------------
    print("\n== target-specific: separate tiny models per target vs shared (res_e1, h16)")
    tspec = {}
    for loss in ["mae", "huber", "quantile"]:
        for asym_thr in [1.0, 3.0]:
            def fp(tr, te, seed, loss=loss, asym_thr=asym_thr):
                fi, va = inner_split(tr, runs, seed)
                T = Y - mech_e1
                P = np.zeros((len(te), len(TARGETS)))
                for j in range(len(TARGETS)):
                    asym = [asym_thr if TARGETS[j] == "throttled_cores" else 1.0]
                    p = fit_mlp(X[fi], T[fi][:, [j]], 8, 3e-3, 1e-4, 32, loss, seed, Xva=X[va], Yva=T[va][:, [j]], asym=asym)
                    P[:, j] = p(X[te])[:, 0]
                return P + mech_e1[te]
            r = agg(cv(lambda tr, te: fp(tr, te, 1), lofo))
            tspec[f"separate_h8/{loss}/asym_throttled={asym_thr:g}"] = r
            print(f"  separate h8 {loss:8s} asym={asym_thr:g}: core={r['core']:.3f} " +
                  " ".join(f"{t[:5]}={r['mean'][t]:.3f}" for t in TARGETS))
    res["target_specific"] = tspec

    # ---- 4. gating policies A-F --------------------------------------------
    print("\n== gating policies (LOFO)")
    # Base learners for gating: direct MLP (B) and residual-on-E1 MLP (C), best configs from the sweep.
    def best_cfg(tag):
        k = sorted((v["core"], k) for k, v in sweep.items() if k.endswith("/" + tag) and k.startswith("mlp"))[0][1]
        parts = k.split("/")[0].split("_")
        return int(parts[1][1:]), (float(parts[2][2:]), float(parts[3][2:]), int(parts[4][1:]), parts[5]), k
    hB, cB, kB = best_cfg("direct")
    hC, cC, kC = best_cfg("res_e1")
    lo_hi = {}
    gates = {k: {} for k in ["A_mechanistic", "B_learned", "C_residual", "D_rule_gate", "E_learned_gate",
                             "F_abstain_on_disagreement", "A_on_F_accepted", "psi_C_residual_D_gated"]}
    abst = {}
    for fname, tr, te in lofo:
        fi, va = inner_split(tr, runs, 1)
        pB = fit_mlp(X[fi], Y[fi], hB, *cB, seed=1, Xva=X[va], Yva=Y[va])
        pC = fit_mlp(X[fi], (Y - mech_e1)[fi], hC, *cC, seed=1, Xva=X[va], Yva=(Y - mech_e1)[va])
        PA, PB, PC = mech_e1[te], pB(X[te]), pC(X[te]) + mech_e1[te]
        # OOD score from the training fold only.
        sx = Std().fit(X[tr])
        lo, hi = X[tr].min(0), X[tr].max(0)
        margin = 0.05 * (hi - lo) + 1e-6
        sc_tr = (sx(X[va]) ** 2).mean(1)
        thr = float(np.quantile(sc_tr, 0.95))
        sc = (sx(X[te]) ** 2).mean(1)
        out_rng = ((X[te] < lo - margin) | (X[te] > hi + margin)).any(1)
        ood = out_rng | (sc > thr)
        resid_mag = np.abs(PC - PA)
        # D: apply the residual only in-distribution and when it is a small correction.
        delta = np.quantile(np.abs(pC(X[va])), 0.9, axis=0)
        use = (~ood)[:, None] & (resid_mag <= delta[None, :])
        PD = np.where(use, PC, PA)
        # E: logistic gate on in-fold validation runs: does the residual help this episode?
        Fva = np.column_stack([(sx(X[va]) ** 2).mean(1), np.abs(pC(X[va])).sum(1),
                               np.abs(pB(X[va]) - mech_e1[va]).sum(1)])
        helps = (np.abs(Y[va] - (pC(X[va]) + mech_e1[va])).mean(1) < np.abs(Y[va] - mech_e1[va]).mean(1)).astype(int)
        if helps.min() == helps.max():
            PE = PC if helps[0] == 1 else PA
        else:
            g = LogisticRegression(C=0.1).fit(Fva, helps)
            Fte = np.column_stack([sc, np.abs(PC - PA).sum(1), np.abs(PB - PA).sum(1)])
            PE = np.where((g.predict_proba(Fte)[:, 1] > 0.5)[:, None], PC, PA)
        # F: abstain when the learned and mechanistic predictions disagree by more than the
        # 90th percentile of their in-fold disagreement; else use the residual model.
        dis = np.abs(PB - PA)[:, :2].sum(1)
        tau = float(np.quantile(np.abs(pB(X[va]) - mech_e1[va])[:, :2].sum(1), 0.9))
        keep = dis <= tau
        for k, P in [("A_mechanistic", PA), ("B_learned", PB), ("C_residual", PC), ("D_rule_gate", PD),
                     ("E_learned_gate", PE)]:
            gates[k][fname] = norm_mae(Y, P, te, scale)
        gates["F_abstain_on_disagreement"][fname] = norm_mae(Y[te][keep], PC[keep], np.arange(keep.sum()), scale) if keep.any() else None
        # Fair comparison for F: the mechanistic model on the same accepted episodes.
        gates["A_on_F_accepted"][fname] = norm_mae(Y[te][keep], PA[keep], np.arange(keep.sum()), scale) if keep.any() else None
        # Composite actually considered for shipping: mechanistic usage/throttling,
        # learned PSI residual only when the rule gate D accepts it.
        PM = PA.copy()
        psi_j = TARGETS.index("psi_some")
        PM[:, psi_j] = np.where(use[:, psi_j], PC[:, psi_j], PA[:, psi_j])
        gates["psi_C_residual_D_gated"][fname] = norm_mae(Y, PM, te, scale)
        abst[fname] = {"F_abstain_rate": float(1 - keep.mean()), "ood_rate": float(ood.mean()),
                       "D_residual_applied_rate": float(use.mean())}
    gate_res = {}
    for k, v in gates.items():
        vv = {f: x for f, x in v.items() if x is not None}
        gate_res[k] = agg(vv)
        print(f"  {k:28s} core={gate_res[k]['core']:.3f} worst={gate_res[k]['core_worst']:.3f} "
              f"ratio={gate_res[k]['mean']['throttled_ratio']:.3f} psi={gate_res[k]['mean']['psi_some']:.3f} | " +
              " ".join(f"{f}={np.mean([vv[f]['usage_cores'], vv[f]['throttled_cores']]):.2f}" for f in vv))
    res["gating"] = {"policies": gate_res, "rates": abst, "base_learners": {"B": kB, "C": kC}}
    print("  abstention/OOD rates per held-out family:", json.dumps(abst))

    res["wall_time_s"] = time.time() - t0
    json.dump(res, open(os.path.join(a.out, "dev_selection.json"), "w"), indent=1, default=float)
    print(f"\nwrote {a.out}/dev_selection.json in {res['wall_time_s']:.0f}s")


if __name__ == "__main__":
    main()
