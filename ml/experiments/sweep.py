# SPDX-License-Identifier: Apache-2.0
"""Model-selection sweep. Selection uses ONLY the family-disjoint dev split.

  python3 ml/experiments/sweep.py --name reg  [--configs reg|size|all]
Writes ml/experiments/results/sweep_<name>.json
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EVAL = os.path.join(ROOT, "build", "osie_eval")
RUNS = os.path.join(ROOT, "ml", "experiments", "runs")
RES = os.path.join(ROOT, "ml", "experiments", "results")

BASE = dict(arch="bigru", buckets=16384, dim=48, hidden=64)
CONFIGS = {
    "reg": [
        dict(BASE, tag="gru1m_d0.2"),
        dict(BASE, tag="gru1m_d0.4", dropout=0.4),
        dict(BASE, tag="gru1m_wd0.15", dropout=0.3, word_dropout=0.15),
        dict(BASE, tag="gru1m_wd0.15_fd0.2", dropout=0.3, word_dropout=0.15, feat_dropout=0.2),
        dict(BASE, tag="gru1m_wd0.25_fd0.3", dropout=0.4, word_dropout=0.25, feat_dropout=0.3),
    ],
    "arch": [
        dict(arch="bag", buckets=16384, dim=48, hidden=64, tag="bag_d48", dropout=0.3, word_dropout=0.15),
        dict(arch="cnn", buckets=16384, dim=48, hidden=64, tag="cnn_d48", dropout=0.3, word_dropout=0.15),
        dict(BASE, tag="gru_d48", dropout=0.3, word_dropout=0.15),
    ],
    "size": [
        dict(arch="bigru", buckets=1024, dim=16, hidden=16, tag="gru_20k", dropout=0.3, word_dropout=0.15),
        dict(arch="bigru", buckets=4096, dim=24, hidden=32, tag="gru_110k", dropout=0.3, word_dropout=0.15),
        dict(arch="bigru", buckets=8192, dim=32, hidden=48, tag="gru_290k", dropout=0.3, word_dropout=0.15),
        dict(arch="bigru", buckets=16384, dim=48, hidden=64, tag="gru_840k", dropout=0.3, word_dropout=0.15),
        dict(arch="bigru", buckets=49152, dim=56, hidden=96, tag="gru_2.9m", dropout=0.3, word_dropout=0.15),
        dict(arch="bigru", buckets=131072, dim=72, hidden=128, tag="gru_9.6m", dropout=0.3, word_dropout=0.15),
    ],
}


def run(cfg: dict, data_dir: str, extra: list[str]) -> dict:
    out = os.path.join(RUNS, cfg["tag"] + ".osiem")
    cmd = [sys.executable, os.path.join(ROOT, "ml/training/train.py"), "--out", out, "--data", data_dir, "--quantize"]
    for k, v in cfg.items():
        if k != "tag":
            cmd += [f"--{k.replace('_', '-')}", str(v)]
    cmd += extra
    subprocess.run(cmd, check=True, capture_output=True)
    rep = json.load(open(out.replace(".osiem", ".json")))
    rep.pop("history", None)
    rep["tag"] = cfg["tag"]
    for parser in ("neural", "hybrid"):
        r = subprocess.run([EVAL, "--data", os.path.join(data_dir, "dev.tsv"), "--parser", parser, "--model", out, "--json"],
                           capture_output=True, check=True)
        rep[f"dev_{parser}"] = json.loads(r.stdout)
    return rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--configs", default="reg")
    ap.add_argument("--data", default=os.path.join(ROOT, "ml/datasets/out"))
    ap.add_argument("--extra", nargs="*", default=[])
    args = ap.parse_args()
    os.makedirs(RUNS, exist_ok=True)
    os.makedirs(RES, exist_ok=True)
    rows = []
    for cfg in CONFIGS[args.configs]:
        r = run(cfg, args.data, args.extra)
        rows.append(r)
        n, h = r["dev_neural"], r["dev_hybrid"]
        print(f"{r['tag']:<22} params={r['params']:>9,} train={r['train_seconds']:5.1f}s epochs={r['epochs_run']:2d} "
              f"dev e2e neural={n['e2e_acc']:5.1f} hybrid={h['e2e_acc']:5.1f} intent={n['intent_acc']:5.1f} "
              f"lat={n['latency_us_mean']:6.1f}us parity={r['parity']['max_abs_diff']:.1e}", flush=True)
    json.dump(rows, open(os.path.join(RES, f"sweep_{args.name}.json"), "w"), indent=2)


if __name__ == "__main__":
    main()
