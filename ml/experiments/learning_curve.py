# SPDX-License-Identifier: Apache-2.0
"""Step-equalised learning curve for the dev-selected configuration (3 seeds)."""
import json, os, statistics, sys
sys.path.insert(0, os.path.dirname(__file__))
import run_all as ra  # noqa: E402

def main():
    summary = json.load(open(os.path.join(ra.RES, "summary.json")))
    best = summary["selected"]
    cfg = ra.CONFIGS[best]
    splits = {"dev": os.path.join(ra.DATA, "dev.tsv"), "test": os.path.join(ra.DATA, "test.tsv"), "hand": ra.HAND}
    curve = {}
    for frac in (0.05, 0.1, 0.25, 0.5, 1.0):
        vals = {f"{p}_{n}": [] for p in ("neural", "hybrid") for n in splits}
        steps = []
        for seed in ra.SEEDS:
            path, rep = ra.train(f"{best}_lc{frac}", cfg, seed, extra=["--train-fraction", str(frac)])
            steps.append(rep["epochs_run"])
            for p in ("neural", "hybrid"):
                for n, sp in splits.items():
                    vals[f"{p}_{n}"].append(ra.evaluate(path, p, sp)["e2e_acc"])
        curve[str(frac)] = {"examples": rep["train_examples"], "epochs_run": steps, **{k: ra.agg(v) for k, v in vals.items()}}
        print(f"frac={frac} n={rep['train_examples']} epochs={steps} " + " ".join(
            f"{k}={statistics.mean(v):.1f}" for k, v in vals.items()), flush=True)
    summary["learning_curve_step_equalised"] = curve
    summary["learning_curve_note"] = ("'learning_curve' used a fixed epoch budget and collapsed for <=10% data; "
                                      "'learning_curve_step_equalised' scales epochs/patience by 1/fraction")
    json.dump(summary, open(os.path.join(ra.RES, "summary.json"), "w"), indent=2)

if __name__ == "__main__":
    main()
