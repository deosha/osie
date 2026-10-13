#!/usr/bin/env python3
"""Secondary analysis (NOT pre-registered): the throttling assessment each
model *would* give if its out-of-distribution checks were ignored, from the
forced intervals, against observed throttling. Answers whether
regime-conditional intervals keep false-safe low where the OOD check
withheld every prediction (GitHub runners: 4 host CPUs, unlike training).

    python3 secondary_assessment.py --data data/gha/wm-ubuntu-24.04 data/gha/wm-ubuntu-24.04-arm data/confirm-local
"""
import argparse
import json

import numpy as np

import v02_confirm as vc
import wm

SAFE = vc.SAFE_MAX


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", nargs="+", required=True)
    ap.add_argument("--out", default="results/v02/secondary_assessment.json")
    a = ap.parse_args()
    models = {"A": "../../models/world_cpu.osiewm", "B": "results/v02/world_cpu_v02b_mondrian.osiewm"}
    out = {}
    for d in a.data:
        eps = []
        for sub in ("", "main", "batch"):
            p = f"{d}/{sub}/episodes.jsonl".replace("//", "/")
            try:
                eps += [e for e in wm.load_episodes(p) if e["valid"]]
            except FileNotFoundError:
                pass
        eps = [e for e in eps if e["action"]["kind"] in ("cpu_quota", "cpuset")]
        y = np.array([wm.mean_field(e["post"], "throttled_frac") for e in eps])
        for name, path in models.items():
            pr = vc.lab_predict(path, eps)
            hi = np.array([pr[i]["targets"]["throttled_cores"]["intervals"][1][2] for i in range(len(eps))])
            lo = np.array([pr[i]["targets"]["throttled_cores"]["intervals"][1][1] for i in range(len(eps))])
            low = hi <= SAFE
            likely = lo > SAFE
            bad = low & (y > SAFE)
            key = f"{d.rstrip('/').split('/')[-1]}/{name}"
            out[key] = {"changes": int(len(eps)), "low": int(low.sum()), "false_safe": int(bad.sum()),
                        "false_safe_rate_among_low": float(bad.sum() / low.sum()) if low.any() else None,
                        "likely": int(likely.sum()), "possible": int((~low & ~likely).sum()),
                        "observed_throttled_over_safe": int((y > SAFE).sum())}
            print(key, out[key])
    json.dump(out, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
