# SPDX-License-Identifier: Apache-2.0
"""Train a tiny joint intent + slot model and export it to OSIE's .osiem format.

  python3 ml/training/train.py --arch bigru --buckets 16384 --dim 48 --hidden 64 \
      --out models/gru_1m.osiem

Architectures (all CPU-friendly, all supported by the C++ inference path):
  bag   hashed char-n-gram embedding bag -> per-token ReLU projection (no context)
  cnn   embedding bag -> 2 x Conv1d(k=3) + ReLU
  bigru embedding bag -> bidirectional GRU
Heads: intent from [mean ; max] pooling, BIO tags per token.

Measured and written to <out>.json: parameters, artifact bytes, estimated
training FLOPs, wall time, peak RSS, dev metrics, C++ parity.
"""
from __future__ import annotations

import argparse
import json
import os
import resource
import struct
import subprocess
import sys
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from osie_ml.text import INTENTS, TAGS, token_features, tokenize  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


# ----------------------------------------------------------------- data
def load_jsonl(path: str):
    with open(path) as f:
        return [json.loads(line) for line in f]


class Featurizer:
    def __init__(self, buckets: int, nmin: int, nmax: int, knowledge: bool = False):
        self.buckets, self.nmin, self.nmax, self.knowledge = buckets, nmin, nmax, knowledge
        self.cache: dict[str, list[int]] = {}

    def __call__(self, tok_text: str) -> list[int]:
        f = self.cache.get(tok_text)
        if f is None:
            f = token_features(tok_text, self.buckets, self.nmin, self.nmax, self.knowledge)
            self.cache[tok_text] = f
        return f


def encode(rows, feat: Featurizer):
    out = []
    for r in rows:
        toks = tokenize(r["text"])
        if not toks:
            continue
        tags = r.get("tags") or ["O"] * len(toks)
        assert len(tags) == len(toks), (r["text"], tags)
        out.append(([feat(t.text) for t in toks], INTENTS.index(r["intent"]), [TAGS.index(t) for t in tags]))
    return out


def augment(item, rng, word_p: float, feat_p: float):
    """Training-time regularisation against template memorisation."""
    if word_p <= 0 and feat_p <= 0:
        return item
    toks, intent, tags = item
    keep = [i for i, t in enumerate(tags) if not (t == 0 and rng.random() < word_p)]
    if not keep:
        keep = list(range(len(toks)))
    new_toks = []
    for i in keep:
        f = toks[i]
        if feat_p > 0 and len(f) > 2:
            kept = [x for x in f if rng.random() >= feat_p]
            f = kept if kept else f[:1]
        new_toks.append(f)
    new_tags = [tags[i] for i in keep]
    # repair BIO after deletions: an I- tag whose predecessor is not its type becomes B-
    for i, t in enumerate(new_tags):
        name = TAGS[t]
        if name.startswith("I-") and (i == 0 or TAGS[new_tags[i - 1]][2:] != name[2:]):
            new_tags[i] = TAGS.index("B-" + name[2:])
    return new_toks, intent, new_tags


def batchify(items):
    """-> flat feature ids, per-token offsets, token->sentence layout (B, T), masks, labels."""
    B = len(items)
    T = max(len(x[0]) for x in items)
    ids, offsets, lengths = [], [], []
    tok_index = torch.full((B, T), -1, dtype=torch.long)
    k = 0
    for b, (toks, _, _) in enumerate(items):
        for t, feats in enumerate(toks):
            offsets.append(len(ids))
            ids.extend(feats)
            tok_index[b, t] = k
            k += 1
        lengths.append(len(toks))
    intents = torch.tensor([x[1] for x in items])
    tags = torch.full((B, T), -100, dtype=torch.long)
    for b, (_, _, tg) in enumerate(items):
        tags[b, : len(tg)] = torch.tensor(tg)
    return (torch.tensor(ids), torch.tensor(offsets), tok_index, torch.tensor(lengths), intents, tags)


# ----------------------------------------------------------------- model
class TinyIntentModel(nn.Module):
    def __init__(self, arch: str, buckets: int, dim: int, hidden: int, n_intents: int, n_tags: int, dropout=0.2):
        super().__init__()
        self.arch = arch
        self.emb = nn.EmbeddingBag(buckets, dim, mode="mean")
        nn.init.normal_(self.emb.weight, std=0.1)
        self.drop = nn.Dropout(dropout)
        if arch == "bigru":
            self.rnn = nn.GRU(dim, hidden, batch_first=True, bidirectional=True)
            enc = 2 * hidden
        elif arch == "cnn":
            self.conv1 = nn.Conv1d(dim, hidden, 3, padding=1)
            self.conv2 = nn.Conv1d(hidden, hidden, 3, padding=1)
            enc = hidden
        elif arch == "bag":
            self.proj = nn.Linear(dim, hidden)
            enc = hidden
        else:
            raise ValueError(arch)
        self.enc = enc
        self.intent = nn.Linear(2 * enc, n_intents)
        self.tag = nn.Linear(enc, n_tags)

    def forward(self, ids, offsets, tok_index, lengths):
        tok_emb = self.emb(ids, offsets)  # [Ntok, D]
        B, T = tok_index.shape
        x = torch.zeros(B, T, tok_emb.shape[1])
        mask = tok_index >= 0
        x[mask] = tok_emb[tok_index[mask]]
        x = self.drop(x)
        if self.arch == "bigru":
            packed = nn.utils.rnn.pack_padded_sequence(x, lengths, batch_first=True, enforce_sorted=False)
            h, _ = self.rnn(packed)
            h, _ = nn.utils.rnn.pad_packed_sequence(h, batch_first=True, total_length=T)
        elif self.arch == "cnn":
            m = mask.unsqueeze(1).float()
            h = F.relu(self.conv1(x.transpose(1, 2)) ) * m
            h = (F.relu(self.conv2(h)) * m).transpose(1, 2)
        else:
            h = F.relu(self.proj(x))
        h = h * mask.unsqueeze(-1)
        mean = h.sum(1) / lengths.unsqueeze(1).float()
        mx = h.masked_fill(~mask.unsqueeze(-1), -1e30).max(1).values
        pooled = torch.cat([mean, mx], dim=1)
        return self.intent(self.drop(pooled)), self.tag(self.drop(h))


def param_count(m: nn.Module) -> int:
    return sum(p.numel() for p in m.parameters())


def forward_flops_per_token(arch, dim, hidden, n_intents, n_tags, feats_per_token=20):
    """Analytic multiply-add count x2 (FLOPs) for one token (heads amortised per token)."""
    f = feats_per_token * dim  # embedding-bag accumulation
    if arch == "bigru":
        f += 2 * 2 * 3 * hidden * (dim + hidden)
        enc = 2 * hidden
    elif arch == "cnn":
        f += 2 * (3 * dim * hidden + 3 * hidden * hidden)
        enc = hidden
    else:
        f += 2 * dim * hidden
        enc = hidden
    f += 2 * enc * n_tags + 2 * 2 * enc * n_intents / 8  # intent head once per ~8-token sentence
    return f


# ----------------------------------------------------------------- export
def export_osiem(model: TinyIntentModel, args, path: str, quantize: bool) -> int:
    sd = {k: v.detach().cpu().numpy().astype(np.float32) for k, v in model.state_dict().items()}
    tensors = [("emb", sd["emb.weight"])]
    if args.arch == "bigru":
        for d, suf in (("f", ""), ("b", "_reverse")):
            tensors += [(f"gru_{d}_wih", sd[f"rnn.weight_ih_l0{suf}"]), (f"gru_{d}_whh", sd[f"rnn.weight_hh_l0{suf}"]),
                        (f"gru_{d}_bih", sd[f"rnn.bias_ih_l0{suf}"]), (f"gru_{d}_bhh", sd[f"rnn.bias_hh_l0{suf}"])]
    elif args.arch == "cnn":
        tensors += [("conv1_w", sd["conv1.weight"]), ("conv1_b", sd["conv1.bias"]),
                    ("conv2_w", sd["conv2.weight"]), ("conv2_b", sd["conv2.bias"])]
    else:
        tensors += [("proj_w", sd["proj.weight"]), ("proj_b", sd["proj.bias"])]
    tensors += [("intent_w", sd["intent.weight"]), ("intent_b", sd["intent.bias"]),
                ("tag_w", sd["tag.weight"]), ("tag_b", sd["tag.bias"])]
    header = ["OSIEM1", f"arch {args.arch}", f"buckets {args.buckets}", f"dim {args.dim}", f"hidden {args.hidden}",
              f"ngram {args.nmin} {args.nmax}", f"knowledge {int(args.knowledge)}", "intents " + " ".join(INTENTS), "tags " + " ".join(TAGS)]
    blobs = []
    for name, arr in tensors:
        q = quantize and name == "emb"
        header.append(f"tensor {name} {'i8' if q else 'f32'} " + " ".join(str(d) for d in arr.shape))
        if q:
            scale = np.maximum(np.abs(arr).max(axis=1), 1e-12) / 127.0
            qa = np.clip(np.round(arr / scale[:, None]), -127, 127).astype(np.int8)
            b = scale.astype("<f4").tobytes() + qa.tobytes()
            b += b"\0" * ((-len(b)) % 4)
            blobs.append(b)
        else:
            blobs.append(arr.astype("<f4").tobytes())
    header.append("data")
    data = ("\n".join(header) + "\n").encode() + b"".join(blobs)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


# ----------------------------------------------------------------- train
def evaluate(model, data, bs=256):
    model.eval()
    ci = ct = nt = 0
    with torch.no_grad():
        for i in range(0, len(data), bs):
            ids, offs, ti, lens, yi, yt = batchify(data[i : i + bs])
            li, lt = model(ids, offs, ti, lens)
            ci += (li.argmax(1) == yi).sum().item()
            m = yt != -100
            ct += (lt.argmax(-1)[m] == yt[m]).sum().item()
            nt += m.sum().item()
    return ci / len(data), ct / max(nt, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="bigru", choices=["bag", "cnn", "bigru"])
    ap.add_argument("--buckets", type=int, default=16384)
    ap.add_argument("--dim", type=int, default=48)
    ap.add_argument("--hidden", type=int, default=64)
    ap.add_argument("--nmin", type=int, default=2)
    ap.add_argument("--nmax", type=int, default=4)
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--patience", type=int, default=4)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--train-fraction", type=float, default=1.0)
    ap.add_argument("--data", default=os.path.join(ROOT, "ml/datasets/out"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--quantize", action="store_true", help="also export an int8-embedding variant")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--dropout", type=float, default=0.2)
    ap.add_argument("--knowledge", action="store_true", help="add OS-lexicon category features")
    ap.add_argument("--word-dropout", type=float, default=0.0, help="drop whole 'O' tokens during training")
    ap.add_argument("--feat-dropout", type=float, default=0.0, help="drop hashed n-gram features during training")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    torch.set_num_threads(args.threads)
    rng = np.random.default_rng(args.seed)

    t_prep = time.time()
    feat = Featurizer(args.buckets, args.nmin, args.nmax, args.knowledge)
    train_rows = load_jsonl(os.path.join(args.data, "train.jsonl"))
    if args.train_fraction < 1.0:
        idx = rng.permutation(len(train_rows))[: max(1, int(len(train_rows) * args.train_fraction))]
        train_rows = [train_rows[i] for i in sorted(idx)]
    train = encode(train_rows, feat)
    dev = encode(load_jsonl(os.path.join(args.data, "dev.jsonl")), feat)
    prep_s = time.time() - t_prep

    if args.train_fraction < 1.0:
        # Equalise optimisation steps: small subsets otherwise get too few updates
        # before early stopping (an artefact, not a sample-efficiency result).
        scale = 1.0 / args.train_fraction
        args.epochs = int(min(400, round(args.epochs * scale)))
        args.patience = int(min(80, round(args.patience * scale)))
    model = TinyIntentModel(args.arch, args.buckets, args.dim, args.hidden, len(INTENTS), len(TAGS), args.dropout)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    tokens_per_epoch = sum(len(x[0]) for x in train)
    avg_feats = float(np.mean([len(f) for x in train for f in x[0]]))
    fwd_flops = forward_flops_per_token(args.arch, args.dim, args.hidden, len(INTENTS), len(TAGS), avg_feats)

    best, best_state, bad, epochs_run = -1.0, None, 0, 0
    t0 = time.time()
    history = []
    for epoch in range(args.epochs):
        model.train()
        order = rng.permutation(len(train))
        for i in range(0, len(order), args.batch):
            batch = [augment(train[j], rng, args.word_dropout, args.feat_dropout) for j in order[i : i + args.batch]]
            ids, offs, ti, lens, yi, yt = batchify(batch)
            li, lt = model(ids, offs, ti, lens)
            loss = F.cross_entropy(li, yi) + F.cross_entropy(lt.reshape(-1, lt.shape[-1]), yt.reshape(-1), ignore_index=-100)
            opt.zero_grad()
            loss.backward()
            opt.step()
        epochs_run += 1
        ia, ta = evaluate(model, dev)
        score = ia + ta
        history.append({"epoch": epoch + 1, "dev_intent_acc": ia, "dev_tag_acc": ta, "loss": float(loss.detach())})
        if score > best:
            best, bad = score, 0
            best_state = {k: v.clone() for k, v in model.state_dict().items()}
        else:
            bad += 1
            if bad >= args.patience:
                break
    train_s = time.time() - t0
    model.load_state_dict(best_state)
    ia, ta = evaluate(model, dev)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    size = export_osiem(model, args, args.out, quantize=False)
    qsize = None
    if args.quantize:
        qsize = export_osiem(model, args, args.out.replace(".osiem", ".int8.osiem"), quantize=True)

    # Golden parity: PyTorch vs the C++ forward pass on dev sentences.
    parity = None
    exe = os.path.join(ROOT, "build", "osie_eval")
    if os.path.exists(exe):
        sample = load_jsonl(os.path.join(args.data, "dev.jsonl"))[:200]
        tmp = args.out + ".parity.tsv"
        with open(tmp, "w") as f:
            f.write("id\tgroup\ttext\tintent\ttarget_kind\ttarget_value\tconcept\taction\n")
            for i, r in enumerate(sample):
                f.write(f"p{i}\tparity\t{r['text']}\t{r['intent']}\thost\t\t\t\n")
        out = subprocess.run([exe, "--dump-probs", "--model", args.out, "--data", tmp], capture_output=True, check=True)
        cpp = np.array([[float(x) for x in line.split()[1:]] for line in out.stdout.decode().splitlines()])
        model.eval()
        enc = encode([{"text": r["text"], "intent": r["intent"]} for r in sample], feat)
        with torch.no_grad():
            ids, offs, ti, lens, _, _ = batchify(enc)
            py = torch.softmax(model(ids, offs, ti, lens)[0], 1).numpy()
        parity = {"max_abs_diff": float(np.abs(cpp - py).max()), "argmax_agreement": float((cpp.argmax(1) == py.argmax(1)).mean())}
        os.remove(tmp)

    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    rss_bytes = rss if sys.platform == "darwin" else rss * 1024
    report = {
        "arch": args.arch, "buckets": args.buckets, "dim": args.dim, "hidden": args.hidden,
        "ngram": [args.nmin, args.nmax], "params": param_count(model), "artifact_bytes": size,
        "artifact_bytes_int8": qsize, "train_examples": len(train), "train_fraction": args.train_fraction,
        "epochs_run": epochs_run, "tokens_per_epoch": tokens_per_epoch, "avg_features_per_token": avg_feats,
        "train_flops_est": 3.0 * fwd_flops * tokens_per_epoch * epochs_run,
        "forward_flops_per_token_est": fwd_flops, "train_seconds": train_s, "prep_seconds": prep_s,
        "peak_rss_bytes": rss_bytes, "threads": args.threads, "dropout": args.dropout,
        "word_dropout": args.word_dropout, "feat_dropout": args.feat_dropout, "knowledge": args.knowledge, "dev_intent_acc": ia, "dev_tag_acc": ta,
        "history": history, "parity": parity, "torch": torch.__version__, "seed": args.seed,
    }
    with open(args.out.replace(".osiem", ".json"), "w") as f:
        json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k != "history"}, indent=None))


if __name__ == "__main__":
    main()
