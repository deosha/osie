#!/usr/bin/env python3
"""Scores intent parsers on the v0.2 evaluation sets.

    python3 ml/datasets/v02/evaluate.py --split test [--parser rules|hybrid|neural --model M] [--out R.json]

Frames come from `osie_eval --frames`, i.e. the same tokenizer, parser and
deterministic frame builder that the engine uses. A prediction counts as the
gold intent when it equals the primary label or one of the `alt_intents` the
annotator marked as defensible (recorded disagreement)."""
import argparse, csv, json, os, subprocess, sys, tempfile
from collections import defaultdict
import random

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(split):
    with open(os.path.join(HERE, f"{split}.tsv")) as f:
        return list(csv.DictReader(f, delimiter="\t"))


def frames(rows, parser, model, binary):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tf:
        for r in rows:
            tf.write(r["text"].replace("\n", " ") + "\n")
        path = tf.name
    cmd = [binary, "--frames", path, "--parser", parser] + (["--model", model] if model else [])
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()
    os.unlink(path)
    assert len(out) == len(rows), (len(out), len(rows))
    return [json.loads(l) for l in out]


def norm(s):
    return " ".join(s.lower().split())


def score(rows, fr):
    res = []
    for r, f in zip(rows, fr):
        gold = r["intent"]
        alts = {a for a in r["alt_intents"].split(",") if a}
        pred = f["intent"]
        i_ok = pred == gold or pred in alts
        gk, gv = r["target_kind"], r["target_value"]
        pk, pv = f["target"]["kind"], f["target"].get("value", "")
        # Host-level and no-target frames are equivalent for scoring entities.
        gent = gk in ("process_name", "pid", "reference")
        pent = pk in ("process_name", "pid", "reference")
        t_ok = (not gent and not pent) or (gent and pent and gk == pk and (gk == "reference" or norm(gv) == norm(pv)))
        a_ok = not r["action"] or f.get("action", "") == r["action"]
        gt = r["temporal"]
        pt = f.get("temporal", {}).get("kind", "") if f.get("temporal") else ""
        tm_ok = gt == pt
        res.append(dict(id=r["id"], family=r["family"], domain=r["domain"], safety=r["safety"], text=r["text"],
                        gold=gold, alts=sorted(alts), pred=pred, i_ok=i_ok, gent=gent, pent=pent, t_ok=t_ok,
                        a_ok=a_ok, tm_ok=tm_ok, e2e=i_ok and t_ok and a_ok and tm_ok,
                        pred_target=f"{pk}:{pv}", gold_target=f"{gk}:{gv}", pred_action=f.get("action", ""),
                        latency_us=f.get("latency_us", 0.0)))
    return res


def macro_f1(res):
    labels = sorted({r["gold"] for r in res} | {r["pred"] for r in res})
    f1s = {}
    for L in labels:
        # A prediction accepted via alt_intents counts as the gold label.
        tp = sum(1 for r in res if r["gold"] == L and r["i_ok"])
        fp = sum(1 for r in res if r["pred"] == L and not r["i_ok"])
        fn = sum(1 for r in res if r["gold"] == L and not r["i_ok"])
        if tp + fp + fn == 0:
            continue
        p = tp / (tp + fp) if tp + fp else 0.0
        rc = tp / (tp + fn) if tp + fn else 0.0
        f1s[L] = 2 * p * rc / (p + rc) if p + rc else 0.0
    gold_labels = {r["gold"] for r in res}
    return sum(f1s[L] for L in gold_labels) / len(gold_labels), f1s


def family_ci(res, key, n=2000, seed=0):
    by = defaultdict(list)
    for r in res:
        by[r["family"]].append(r[key])
    fams = list(by)
    rng = random.Random(seed)
    vals = []
    for _ in range(n):
        pick = [fams[rng.randrange(len(fams))] for _ in fams]
        xs = [x for f in pick for x in by[f]]
        vals.append(sum(xs) / len(xs))
    vals.sort()
    return [vals[int(0.025 * n)], vals[int(0.975 * n) - 1]]


def summarize(res):
    n = len(res)
    mf1, per = macro_f1(res)
    ent_pred = [r for r in res if r["pent"]]
    ent_gold = [r for r in res if r["gent"]]
    directives = [r for r in res if r["safety"] == "directive"]
    nondirective_safety = [r for r in res if r["safety"] in ("explain", "past", "hypothetical", "quoted")]
    unsup_gold = [r for r in res if r["gold"] == "unsupported"]
    unsup_pred = [r for r in res if r["pred"] == "unsupported"]
    out = {
        "n": n,
        "families": len({r["family"] for r in res}),
        "intent_acc": sum(r["i_ok"] for r in res) / n,
        "intent_macro_f1": mf1,
        "entity_precision": sum(r["t_ok"] for r in ent_pred) / len(ent_pred) if ent_pred else None,
        "entity_recall": sum(r["t_ok"] for r in ent_gold) / len(ent_gold) if ent_gold else None,
        "e2e_frame_acc": sum(r["e2e"] for r in res) / n,
        "e2e_ci95_by_family": family_ci(res, "e2e"),
        "unsupported_precision": sum(r["gold"] == "unsupported" for r in unsup_pred) / len(unsup_pred) if unsup_pred else None,
        "unsupported_recall": sum(r["pred"] == "unsupported" for r in unsup_gold) / len(unsup_gold) if unsup_gold else None,
        # Safety: a directive not routed to action_request is a false-safe routing
        # (it would be answered as if it were a question).
        "directives": len(directives),
        "false_safe_routing": sum(r["pred"] != "action_request" for r in directives) / len(directives) if directives else None,
        "action_type_acc_on_directives": sum(r["a_ok"] and r["pred"] == "action_request" for r in directives) / len(directives) if directives else None,
        "over_refusal_on_explain_past_hypothetical": sum(r["pred"] == "action_request" for r in nondirective_safety) / len(nondirective_safety) if nondirective_safety else None,
        "temporal_acc": sum(r["tm_ok"] for r in res) / n,
        "latency_us_p50": sorted(r["latency_us"] for r in res)[n // 2],
        "latency_us_p99": sorted(r["latency_us"] for r in res)[min(n - 1, int(0.99 * n))],
    }
    by_dom = defaultdict(list)
    for r in res:
        by_dom[r["domain"]].append(r["e2e"])
    out["e2e_by_domain"] = {k: sum(v) / len(v) for k, v in sorted(by_dom.items())}
    by_fam = defaultdict(list)
    for r in res:
        by_fam[r["family"]].append(r["e2e"])
    out["e2e_by_family"] = {k: round(sum(v) / len(v), 3) for k, v in sorted(by_fam.items())}
    out["worst_family_e2e"] = min(out["e2e_by_family"].values())
    out["per_label_f1"] = {k: round(v, 3) for k, v in sorted(per.items())}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--split", default="test", choices=["test", "dev"])
    ap.add_argument("--parser", default="rules")
    ap.add_argument("--model", default="")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "osie_eval"))
    ap.add_argument("--out", default="")
    ap.add_argument("--errors", action="store_true")
    a = ap.parse_args()
    rows = load(a.split)
    res = score(rows, frames(rows, a.parser, a.model, a.binary))
    s = summarize(res)
    s["parser"] = a.parser
    s["split"] = a.split
    if a.out:
        json.dump({"summary": s, "rows": res}, open(a.out, "w"), indent=1)
    keys = ["n", "intent_acc", "intent_macro_f1", "entity_precision", "entity_recall", "e2e_frame_acc",
            "e2e_ci95_by_family", "unsupported_recall", "false_safe_routing", "action_type_acc_on_directives",
            "over_refusal_on_explain_past_hypothetical", "temporal_acc", "worst_family_e2e", "latency_us_p99"]
    for k in keys:
        v = s[k]
        print(f"{k:45s} {v if not isinstance(v, float) else round(v, 3)}")
    print("e2e_by_domain", {k: round(v, 3) for k, v in s["e2e_by_domain"].items()})
    if a.errors:
        for r in res:
            if not r["e2e"]:
                print(f"  {r['id']} [{r['family']}] {r['text']!r}: gold={r['gold']}/{r['gold_target']}/{r['pred_action'] and ''} "
                      f"pred={r['pred']}/{r['pred_target']}/{r['pred_action']} temporal_ok={r['tm_ok']}")


if __name__ == "__main__":
    main()
