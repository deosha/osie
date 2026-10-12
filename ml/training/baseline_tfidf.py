# SPDX-License-Identifier: Apache-2.0
"""Classical baseline: TF-IDF (word 1-2 + char_wb 2-4) + logistic regression
for intent; slots come from the deterministic rule extractor (C++), so the
end-to-end frame is scored by the same osie_eval pipeline.

  python3 ml/training/baseline_tfidf.py --eval data.tsv [--eval other.tsv]
"""
from __future__ import annotations

import argparse
import json
import os
import pickle
import resource
import subprocess
import sys
import time

import numpy as np
from scipy.sparse import hstack
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.linear_model import LogisticRegression

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def read_tsv(path):
    rows = []
    with open(path) as f:
        header = f.readline().rstrip("\n").split("\t")
        for line in f:
            parts = line.rstrip("\n").split("\t")
            rows.append(dict(zip(header, parts)))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(ROOT, "ml/datasets/out"))
    ap.add_argument("--eval", nargs="+", required=True)
    ap.add_argument("--C", type=float, default=10.0)
    ap.add_argument("--out", default=os.path.join(ROOT, "ml/experiments/results/tfidf.json"))
    args = ap.parse_args()

    train = [json.loads(l) for l in open(os.path.join(args.data, "train.jsonl"))]
    X_text = [r["text"] for r in train]
    y = [r["intent"] for r in train]
    t0 = time.time()
    word = TfidfVectorizer(analyzer="word", ngram_range=(1, 2), lowercase=True, min_df=1, sublinear_tf=True)
    char = TfidfVectorizer(analyzer="char_wb", ngram_range=(2, 4), lowercase=True, min_df=2, sublinear_tf=True)
    X = hstack([word.fit_transform(X_text), char.fit_transform(X_text)]).tocsr()
    clf = LogisticRegression(C=args.C, max_iter=2000)
    clf.fit(X, y)
    train_s = time.time() - t0
    blob = pickle.dumps((word, char, clf))
    n_params = clf.coef_.size + clf.intercept_.size
    report = {"model": "tfidf_word12_char24_logreg", "C": args.C, "features": X.shape[1], "params": int(n_params),
              "artifact_bytes_pickle": len(blob), "train_seconds": train_s,
              "peak_rss_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss, "evals": {}}
    for path in args.eval:
        rows = read_tsv(path)
        texts = [r["text"] for r in rows]
        t1 = time.perf_counter()
        pred = clf.predict(hstack([word.transform(texts), char.transform(texts)]).tocsr())
        batch_us = (time.perf_counter() - t1) / len(texts) * 1e6
        # single-utterance latency (the interactive case)
        lat = []
        for t in texts[:200]:
            s = time.perf_counter()
            clf.predict(hstack([word.transform([t]), char.transform([t])]).tocsr())
            lat.append((time.perf_counter() - s) * 1e6)
        pred_file = path + ".tfidf.pred"
        with open(pred_file, "w") as f:
            f.write("\n".join(pred) + "\n")
        r = subprocess.run([os.path.join(ROOT, "build/osie_eval"), "--data", path, "--parser", "override",
                            "--intents", pred_file, "--json"], capture_output=True, check=True)
        res = json.loads(r.stdout)
        res["python_latency_us_single_mean"] = float(np.mean(lat))
        res["python_latency_us_batched"] = batch_us
        os.remove(pred_file)
        report["evals"][os.path.basename(path)] = res
        print(f"tfidf {os.path.basename(path):<28} intent={res['intent_acc']:5.1f} e2e={res['e2e_acc']:5.1f} "
              f"single={np.mean(lat):.0f}us")
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    json.dump(report, open(args.out, "w"), indent=2)
    print(f"params={n_params} features={X.shape[1]} train={train_s:.1f}s pickle={len(blob)/1e6:.1f}MB")


if __name__ == "__main__":
    main()
