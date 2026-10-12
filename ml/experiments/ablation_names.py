# SPDX-License-Identifier: Apache-2.0
"""Ablation: slot-value (process-name) randomisation during training.

Same protocol as run_all.py (3 seeds, dev-only threshold calibration). If a
configuration beats every run_all configuration on dev (calibrated hybrid),
it is deployed to models/intent.osiem; test/hand scores never influence that.

  python3 ml/experiments/ablation_names.py
"""
from __future__ import annotations

import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
import run_all as ra  # noqa: E402

RATES = [0.5]
CONFIGS = {k: ra.CONFIGS[k] for k in ("gru_300k", "bag_300k_kf", "gru_300k_kf", "gru_840k")}


def main():
    out = {}
    splits = {"dev": os.path.join(ra.DATA, "dev.tsv"), "test": os.path.join(ra.DATA, "test.tsv"), "hand": ra.HAND}
    for rate in RATES:
        ddir = os.path.join(ra.RUNS, f"data_namerand{rate}")
        ra.sh([sys.executable, os.path.join(ra.ROOT, "ml/datasets/generate.py"), "--out", ddir,
               "--name-randomization", str(rate)])
        for tag, cfg in CONFIGS.items():
            rows = []
            for seed in ra.SEEDS:
                path, rep = ra.train(f"{tag}_nr{rate}", cfg, seed, data_dir=ddir)
                th = max(ra.THRESHOLDS, key=lambda t: ra.evaluate(path, "hybrid", splits["dev"], t)["e2e_acc"])
                r = {"threshold": th, "path": path}
                for name, sp in splits.items():  # dev/test/hand of the ORIGINAL split (names unseen)
                    r[f"neural_{name}"] = ra.evaluate(path, "neural", sp)["e2e_acc"]
                    r[f"hybridcal_{name}"] = ra.evaluate(path, "hybrid", sp, th)["e2e_acc"]
                    r[f"neural_{name}_target"] = ra.evaluate(path, "neural", sp)["target_acc"]
                rows.append(r)
            key = f"{tag}_nr{rate}"
            out[key] = {"config": cfg, "name_randomization": rate, "params": rep["params"],
                        "thresholds": [r["threshold"] for r in rows]}
            for m in ("neural", "hybridcal"):
                for name in splits:
                    out[key][f"{m}_{name}_e2e"] = ra.agg([r[f"{m}_{name}"] for r in rows])
            for name in splits:
                out[key][f"neural_{name}_target"] = ra.agg([r[f"neural_{name}_target"] for r in rows])
            c = out[key]
            print(f"{key:<20} dev={c['neural_dev_e2e']['mean']:.1f}/{c['hybridcal_dev_e2e']['mean']:.1f} "
                  f"test={c['neural_test_e2e']['mean']:.1f}/{c['hybridcal_test_e2e']['mean']:.1f} "
                  f"hand={c['neural_hand_e2e']['mean']:.1f}/{c['hybridcal_hand_e2e']['mean']:.1f} "
                  f"target(test)={c['neural_test_target']['mean']:.1f}", flush=True)
    json.dump(out, open(os.path.join(ra.RES, "ablation_names.json"), "w"), indent=2)

    # Dev-only deployment decision across run_all + this ablation.
    summary = json.load(open(os.path.join(ra.RES, "summary.json")))
    cands = {t: c["hybridcal_dev_e2e"]["mean"] for t, c in summary["configs"].items()}
    cands.update({t: c["hybridcal_dev_e2e"]["mean"] for t, c in out.items()})
    best = max(cands, key=cands.get)
    decision = {"candidates_dev_hybridcal": cands, "selected": best}
    if best in out:
        th = out[best]["thresholds"][0]
        src = os.path.join(ra.RUNS, f"{best}_s1")
        for suf in (".osiem", ".int8.osiem", ".json"):
            dst = os.path.join(ra.ROOT, "models", "intent" + suf)
            shutil.copy(src + suf, dst)
            if suf.endswith(".osiem"):
                raw = open(dst, "rb").read()
                raw = raw.replace(b"\ndata\n", f"\nhybrid_threshold {th}\ndata\n".encode(), 1)
                open(dst, "wb").write(raw)
        decision["deployed"] = f"{best}_s1"
    json.dump(decision, open(os.path.join(ra.RES, "deployment_decision.json"), "w"), indent=2)
    print("selected on dev:", best, round(cands[best], 2))


if __name__ == "__main__":
    main()
