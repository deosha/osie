# SPDX-License-Identifier: Apache-2.0
"""Reproduces every intent-model result in docs/RESEARCH.md.

  python3 ml/experiments/run_all.py            (~15-20 min on an M4 Pro, CPU only)

Protocol
  * model selection uses ONLY the family-disjoint dev split;
  * the synthetic test split (template- and lexicon-disjoint) and the
    hand-written held-out set are scored once, for reporting;
  * every neural configuration is trained with 3 seeds (mean +- std);
  * all parsers are scored end-to-end by build/osie_eval through the same
    deterministic C++ frame builder the engine uses.
"""
from __future__ import annotations

import json
import os
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EVAL = os.path.join(ROOT, "build", "osie_eval")
RUNS = os.path.join(ROOT, "ml", "experiments", "runs")
RES = os.path.join(ROOT, "ml", "experiments", "results")
DATA = os.path.join(ROOT, "ml", "datasets", "out")
HAND = os.path.join(ROOT, "ml", "datasets", "heldout_handwritten.tsv")
SEEDS = [1, 2, 3]
REG = dict(dropout=0.3, word_dropout=0.15)

CONFIGS = {
    # matched-size architecture comparison (~0.3M params)
    "bag_300k": dict(arch="bag", buckets=8192, dim=32, hidden=48, **REG),
    "cnn_300k": dict(arch="cnn", buckets=8192, dim=32, hidden=48, **REG),
    "gru_300k": dict(arch="bigru", buckets=8192, dim=32, hidden=48, **REG),
    # size curve (BiGRU)
    "gru_20k": dict(arch="bigru", buckets=1024, dim=16, hidden=16, **REG),
    "gru_110k": dict(arch="bigru", buckets=4096, dim=24, hidden=32, **REG),
    "gru_840k": dict(arch="bigru", buckets=16384, dim=48, hidden=64, **REG),
    "gru_2.9m": dict(arch="bigru", buckets=49152, dim=56, hidden=96, **REG),
    "gru_9.6m": dict(arch="bigru", buckets=131072, dim=72, hidden=128, **REG),
    # tiny context-free model
    "bag_20k": dict(arch="bag", buckets=1024, dim=16, hidden=16, **REG),
    # knowledge-assisted: OS-lexicon category features
    "bag_300k_kf": dict(arch="bag", buckets=8192, dim=32, hidden=48, knowledge=True, **REG),
    "gru_300k_kf": dict(arch="bigru", buckets=8192, dim=32, hidden=48, knowledge=True, **REG),
}
THRESHOLDS = [0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 0.99]


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def evaluate(model: str | None, parser: str, data: str, threshold: float | None = None) -> dict:
    cmd = [EVAL, "--data", data, "--parser", parser, "--json"]
    if model:
        cmd += ["--model", model]
    if threshold is not None:
        cmd += ["--threshold", str(threshold)]
    return json.loads(sh(cmd).stdout)


def train(tag: str, cfg: dict, seed: int, data_dir: str = DATA, extra=None) -> tuple[str, dict]:
    out = os.path.join(RUNS, f"{tag}_s{seed}.osiem")
    cmd = [sys.executable, os.path.join(ROOT, "ml/training/train.py"), "--out", out, "--data", data_dir,
           "--seed", str(seed), "--quantize"]
    for k, v in cfg.items():
        if v is True:
            cmd.append(f"--{k.replace('_', '-')}")
        else:
            cmd += [f"--{k.replace('_', '-')}", str(v)]
    cmd += extra or []
    sh(cmd)
    rep = json.load(open(out.replace(".osiem", ".json")))
    rep.pop("history", None)
    return out, rep


def agg(values):
    return {"mean": statistics.mean(values), "std": statistics.pstdev(values) if len(values) > 1 else 0.0,
            "values": values}


def main():
    t_start = time.time()
    os.makedirs(RUNS, exist_ok=True)
    os.makedirs(RES, exist_ok=True)
    gen = json.loads(sh([sys.executable, os.path.join(ROOT, "ml/datasets/generate.py")]).stdout)
    splits = {"dev": os.path.join(DATA, "dev.tsv"), "test": os.path.join(DATA, "test.tsv"), "hand": HAND}
    summary = {"dataset": gen, "seeds": SEEDS, "configs": {}, "rules": {}, "tfidf": {}}

    for name, path in splits.items():
        summary["rules"][name] = evaluate(None, "rules", path)
    sh([sys.executable, os.path.join(ROOT, "ml/training/baseline_tfidf.py"), "--eval", *splits.values(),
        "--out", os.path.join(RES, "tfidf.json")])
    tf = json.load(open(os.path.join(RES, "tfidf.json")))
    summary["tfidf"] = tf

    for tag, cfg in CONFIGS.items():
        rows = []
        for seed in SEEDS:
            path, rep = train(tag, cfg, seed)
            r = {"train": rep}
            for parser in ("neural", "hybrid"):
                for name, sp in splits.items():
                    r[f"{parser}_{name}"] = evaluate(path, parser, sp)
            # Calibrate the hybrid fallback threshold on dev, then score test/hand with it.
            th = max(THRESHOLDS, key=lambda t: evaluate(path, "hybrid", splits["dev"], t)["e2e_acc"])
            r["threshold"] = th
            for name, sp in splits.items():
                r[f"hybridcal_{name}"] = evaluate(path, "hybrid", sp, th)
            r["int8_neural_dev"] = evaluate(path.replace(".osiem", ".int8.osiem"), "neural", splits["dev"])
            r["int8_hybrid_hand"] = evaluate(path.replace(".osiem", ".int8.osiem"), "hybrid", splits["hand"])
            rows.append(r)
        c = {"config": cfg, "params": rows[0]["train"]["params"], "artifact_bytes": rows[0]["train"]["artifact_bytes"],
             "artifact_bytes_int8": rows[0]["train"]["artifact_bytes_int8"],
             "train_seconds": agg([r["train"]["train_seconds"] for r in rows]),
             "train_flops_est": agg([r["train"]["train_flops_est"] for r in rows]),
             "peak_rss_bytes": agg([r["train"]["peak_rss_bytes"] for r in rows]),
             "parity_max_abs_diff": max(r["train"]["parity"]["max_abs_diff"] for r in rows),
             "latency_us_mean": agg([r["neural_hand"]["latency_us_mean"] for r in rows])}
        for parser in ("neural", "hybrid"):
            for name in splits:
                c[f"{parser}_{name}_e2e"] = agg([r[f"{parser}_{name}"]["e2e_acc"] for r in rows])
                c[f"{parser}_{name}_intent"] = agg([r[f"{parser}_{name}"]["intent_acc"] for r in rows])
        for name in splits:
            c[f"hybridcal_{name}_e2e"] = agg([r[f"hybridcal_{name}"]["e2e_acc"] for r in rows])
        c["thresholds"] = [r["threshold"] for r in rows]
        c["int8_neural_dev_e2e"] = agg([r["int8_neural_dev"]["e2e_acc"] for r in rows])
        c["int8_hybrid_hand_e2e"] = agg([r["int8_hybrid_hand"]["e2e_acc"] for r in rows])
        summary["configs"][tag] = c
        print(f"{tag:<10} params={c['params']:>9,} dev neural={c['neural_dev_e2e']['mean']:.1f}"
              f"+-{c['neural_dev_e2e']['std']:.1f} hybrid={c['hybrid_dev_e2e']['mean']:.1f} "
              f"test={c['neural_test_e2e']['mean']:.1f}/{c['hybrid_test_e2e']['mean']:.1f} "
              f"hand={c['neural_hand_e2e']['mean']:.1f}/{c['hybrid_hand_e2e']['mean']:.1f} "
              f"cal(th={c['thresholds']}) test={c['hybridcal_test_e2e']['mean']:.1f} "
              f"hand={c['hybridcal_hand_e2e']['mean']:.1f}", flush=True)

    # Selection on dev only: best calibrated-hybrid dev mean.
    best = max(summary["configs"], key=lambda t: summary["configs"][t]["hybridcal_dev_e2e"]["mean"])
    summary["selected"] = best
    bcfg = CONFIGS[best]

    # Learning curve (sample efficiency) for the selected configuration.
    curve = {}
    for frac in (0.05, 0.1, 0.25, 0.5, 1.0):
        vals = {"neural_dev": [], "hybrid_dev": [], "neural_test": [], "hybrid_test": [], "examples": 0}
        for seed in SEEDS:
            path, rep = train(f"{best}_frac{frac}", bcfg, seed, extra=["--train-fraction", str(frac)])
            vals["examples"] = rep["train_examples"]
            for parser in ("neural", "hybrid"):
                for name in ("dev", "test"):
                    vals[f"{parser}_{name}"].append(evaluate(path, parser, splits[name])["e2e_acc"])
        curve[str(frac)] = {k: (agg(v) if isinstance(v, list) else v) for k, v in vals.items()}
        print(f"curve frac={frac} n={vals['examples']} dev={statistics.mean(vals['neural_dev']):.1f} "
              f"test={statistics.mean(vals['neural_test']):.1f}", flush=True)
    summary["learning_curve"] = curve

    # Template-diversity ablation: same total budget, fewer distinct families.
    diversity = {}
    for k in (1, 2, 0):
        ddir = os.path.join(RUNS, f"data_fam{k}")
        reps = {1: 90, 2: 45, 0: 30}[k]  # keep the example count roughly constant
        sh([sys.executable, os.path.join(ROOT, "ml/datasets/generate.py"), "--out", ddir,
            "--max-train-families", str(k), "--per-template-train", str(reps)])
        vals = {"neural_dev": [], "neural_test": [], "examples": 0}
        for seed in SEEDS:
            path, rep = train(f"{best}_fam{k}", bcfg, seed, data_dir=ddir)
            vals["examples"] = rep["train_examples"]
            vals["neural_dev"].append(evaluate(path, "neural", splits["dev"])["e2e_acc"])
            vals["neural_test"].append(evaluate(path, "neural", splits["test"])["e2e_acc"])
        diversity["all" if k == 0 else str(k)] = {kk: (agg(v) if isinstance(v, list) else v) for kk, v in vals.items()}
        print(f"families={k or 'all'} n={vals['examples']} dev={statistics.mean(vals['neural_dev']):.1f} "
              f"test={statistics.mean(vals['neural_test']):.1f}", flush=True)
    summary["family_diversity"] = diversity

    # Deploy: selected config, seed 1 (+ int8), and matched bag/cnn models for benchmarks.
    models = os.path.join(ROOT, "models")
    os.makedirs(models, exist_ok=True)
    th_best = summary["configs"][best]["thresholds"][0]
    for src_tag, dst in ((best, "intent"), ("bag_300k", "intent_bag"), ("cnn_300k", "intent_cnn")):
        for suf in (".osiem", ".int8.osiem", ".json"):
            src, dstp = os.path.join(RUNS, f"{src_tag}_s1{suf}"), os.path.join(models, dst + suf)
            shutil.copy(src, dstp)
            if dst == "intent" and suf.endswith(".osiem"):  # embed the dev-calibrated threshold
                raw = open(dstp, "rb").read()
                raw = raw.replace(b"\ndata\n", f"\nhybrid_threshold {th_best}\ndata\n".encode(), 1)
                open(dstp, "wb").write(raw)
    summary["deployed"] = {"intent": f"{best}_s1", "intent_bag": "bag_300k_s1", "intent_cnn": "cnn_300k_s1"}
    summary["wall_seconds"] = time.time() - t_start
    json.dump(summary, open(os.path.join(RES, "summary.json"), "w"), indent=2)
    print(f"selected={best} total {summary['wall_seconds']/60:.1f} min")


if __name__ == "__main__":
    main()
