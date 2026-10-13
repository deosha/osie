# SPDX-License-Identifier: Apache-2.0
"""v0.2 intent classifiers against the rule parser on the frozen v0.2 sets.

    python3 ml/training/compare_v02.py [--final]

Classifiers are trained on the synthetic set (ml/datasets/out_v02, from
generate.py). Every configuration and combination policy is selected on the
v0.2 *dev* set; `--final` additionally scores the selected configurations on
the frozen *test* set once and writes ml/datasets/v02/results/compare_test.json.
Slots, safety routing and frame validation always come from the C++
pipeline (osie_eval --frames), so only the intent decision differs.
"""
import argparse
import json
import os
import sys
import time

import numpy as np
from scipy.sparse import hstack
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.linear_model import LogisticRegression

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "ml", "datasets", "v02"))
import evaluate as ev  # noqa: E402

BIN = os.path.join(ROOT, "build", "osie_eval")
FEATS = {
    "char25": lambda: [TfidfVectorizer(analyzer="char_wb", ngram_range=(2, 5), sublinear_tf=True, min_df=2)],
    "word12_char24": lambda: [TfidfVectorizer(analyzer="word", ngram_range=(1, 2), sublinear_tf=True),
                              TfidfVectorizer(analyzer="char_wb", ngram_range=(2, 4), sublinear_tf=True, min_df=2)],
}


def fit(feat, C, texts, y):
    vs = FEATS[feat]()
    X = hstack([v.fit_transform(texts) for v in vs]).tocsr()
    clf = LogisticRegression(C=C, max_iter=3000)
    t0 = time.time()
    clf.fit(X, y)
    return vs, clf, time.time() - t0


def predict(model, texts):
    vs, clf, _ = model
    X = hstack([v.transform(texts) for v in vs]).tocsr()
    P = clf.predict_proba(X)
    return [clf.classes_[i] for i in P.argmax(1)], P.max(1)


def run(rows, policy, model, rule_frames):
    texts = [r["text"] for r in rows]
    if policy == "rules":
        fr = rule_frames
    else:
        pred, conf = predict(model, texts)
        kind, thr = policy.split(":") if ":" in policy else (policy, "0")
        thr = float(thr)
        out = []
        for p, c, rf in zip(pred, conf, rule_frames):
            if kind == "classifier":
                out.append(p)
            elif kind == "rules_then_clf":  # classifier only where rules give up
                out.append(p if rf["intent"] == "unsupported" and c >= thr else rf["intent"])
            elif kind == "clf_then_rules":  # rules where the classifier is unsure
                out.append(p if c >= thr else rf["intent"])
        fr = ev.frames(rows, "override", "", BIN, intents=out)
    return ev.summarize(ev.score(rows, fr))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--final", action="store_true")
    a = ap.parse_args()
    train = [json.loads(l) for l in open(os.path.join(ROOT, "ml/datasets/out_v02/train.jsonl"))]
    texts, y = [r["text"] for r in train], [r["intent"] for r in train]
    dev = ev.load("dev")
    dev_rules = ev.frames(dev, "rules", "", BIN)
    results = {"dev": {}}
    models = {}
    for feat in FEATS:
        for C in (1.0, 10.0):
            name = f"{feat}_C{C:g}"
            models[name] = fit(feat, C, texts, y)
            for policy in ["classifier", "rules_then_clf:0.5", "clf_then_rules:0.6", "clf_then_rules:0.8",
                           "clf_then_rules:0.9"]:
                s = run(dev, policy, models[name], dev_rules)
                results["dev"][f"{name}|{policy}"] = {k: s[k] for k in ("e2e_frame_acc", "intent_macro_f1",
                                                                        "false_safe_routing", "worst_family_e2e")}
    s = run(dev, "rules", None, dev_rules)
    results["dev"]["rules"] = {k: s[k] for k in ("e2e_frame_acc", "intent_macro_f1", "false_safe_routing",
                                                 "worst_family_e2e")}
    ranked = sorted(results["dev"].items(), key=lambda kv: -kv[1]["e2e_frame_acc"])
    for k, v in ranked[:12]:
        print(f"dev {k:45s} e2e={v['e2e_frame_acc']:.3f} f1={v['intent_macro_f1']:.3f} "
              f"false_safe={v['false_safe_routing']:.3f}")
    best_clf_only = max((kv for kv in results["dev"].items() if kv[0].endswith("|classifier")),
                        key=lambda kv: kv[1]["e2e_frame_acc"])[0]
    best_combo = max((kv for kv in results["dev"].items() if "|" in kv[0] and not kv[0].endswith("|classifier")),
                     key=lambda kv: kv[1]["e2e_frame_acc"])[0]
    results["selected_on_dev"] = {"classifier_only": best_clf_only, "combination": best_combo}
    print("selected on dev:", results["selected_on_dev"])
    out_dir = os.path.join(ROOT, "ml/datasets/v02/results")
    if a.final:
        test = ev.load("test")
        test_rules = ev.frames(test, "rules", "", BIN)
        results["test"] = {"rules": run(test, "rules", None, test_rules)}
        for key in (best_clf_only, best_combo):
            name, policy = key.split("|")
            results["test"][key] = run(test, policy, models[name], test_rules)
        for k, v in results["test"].items():
            print(f"test {k:45s} e2e={v['e2e_frame_acc']:.3f} ci={[round(x, 3) for x in v['e2e_ci95_by_family']]} "
                  f"f1={v['intent_macro_f1']:.3f} false_safe={v['false_safe_routing']:.3f} "
                  f"worst_family={v['worst_family_e2e']:.3f}")
        info = {k: {"params": int(m[1].coef_.size + m[1].intercept_.size), "train_s": m[2]} for k, m in models.items()}
        results["models"] = info
        json.dump(results, open(os.path.join(out_dir, "compare_test.json"), "w"), indent=1, default=float)
    else:
        json.dump(results, open(os.path.join(out_dir, "compare_dev.json"), "w"), indent=1, default=float)


if __name__ == "__main__":
    main()
