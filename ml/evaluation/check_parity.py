# SPDX-License-Identifier: Apache-2.0
"""Checks that Python tokenization/feature hashing is byte-identical to C++.

usage: python3 ml/evaluation/check_parity.py build/osie_eval
"""
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from osie_ml.text import token_features, tokenize  # noqa: E402

CASES = [
    "Why is Chrome's helper_2 using 2.5GB?",
    "what's com.apple.WebKit.WebContent doing -- pid=1234",
    "  kernel_task ... __init__ -x- ",
    "Ünïcödé Prozess läuft? 日本語 ok",
    "Google Chrome Helper (Renderer) at 300%",
    "why is my CPU using so much memory? kill it, the docker cgroup is throttled",
    "",
]


def cpp_features(exe: str, text: str, buckets: int, nmin: int, nmax: int, knowledge: bool):
    cmd = [exe, "--dump-features", text, "--buckets", str(buckets), "--ngram", str(nmin), str(nmax)]
    if knowledge:
        cmd.append("--knowledge")
    out = subprocess.run(cmd, capture_output=True, check=True).stdout.decode("utf-8")
    rows = []
    for line in out.splitlines():
        parts = line.split(" ")
        rows.append((parts[0], [int(x) for x in parts[1:]]))
    return rows


def main() -> int:
    exe = sys.argv[1]
    rng = random.Random(7)
    alphabet = "abcXYZ019 ._-'?!/(),é"
    cases = CASES + ["".join(rng.choice(alphabet) for _ in range(rng.randint(1, 40))) for _ in range(200)]
    bad = 0
    for text in cases:
        if not text.strip(" ?!/(),'"):
            continue
        for buckets, nmin, nmax, kn in ((4096, 2, 4, False), (16384, 1, 3, False), (8192, 2, 4, True)):
            want = cpp_features(exe, text, buckets, nmin, nmax, kn)
            got = [(t.text, token_features(t.text, buckets, nmin, nmax, kn)) for t in tokenize(text)]
            if [w[0] for w in want] != [g[0].split(" ")[0] for g in got] or [w[1] for w in want] != [g[1] for g in got]:
                bad += 1
                print("MISMATCH", repr(text), want[:2], got[:2])
    print(f"parity: {len(cases)} texts, {bad} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
