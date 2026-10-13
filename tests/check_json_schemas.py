#!/usr/bin/env python3
"""Validates OSIE's JSON outputs against docs/schemas/*.schema.json.

    python3 tests/check_json_schemas.py [path/to/osie]

Covers every utterance of the frozen v0.2 test and dev sets (parse --json),
a set of live questions (ask --json) and, on Linux, predict --json."""
import csv
import json
import os
import platform
import subprocess
import sys

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
osie = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "osie")
schemas = {n: json.load(open(os.path.join(ROOT, "docs", "schemas", f"{n}.schema.json"))) for n in ("frame", "answer", "predict")}
registry = Registry().with_resources([(f"{n}.schema.json", Resource.from_contents(s)) for n, s in schemas.items()] +
                                     [(s["$id"], Resource.from_contents(s)) for s in schemas.values()])
V = {n: Draft202012Validator(s, registry=registry) for n, s in schemas.items()}
fails = 0


def check(kind, out, what):
    global fails
    errs = sorted(V[kind].iter_errors(json.loads(out)), key=str)
    if errs:
        fails += 1
        print(f"FAIL {kind} {what!r}: {errs[0].message[:200]}")


n = 0
for split in ("test", "dev"):
    for r in csv.DictReader(open(os.path.join(ROOT, "ml", "datasets", "v02", f"{split}.tsv")), delimiter="\t"):
        check("frame", subprocess.run([osie, "parse", r["text"]], capture_output=True, text=True, check=True).stdout, r["text"])
        n += 1
for q in ["what processes are running?", "why is my machine slow", "kill process 1", "how does kill -9 work?",
          "how many cpus can this container use", "which cgroup is constrained?", "tell me about pid 1",
          "what is memory compression", "write me a poem"]:
    check("answer", subprocess.run([osie, "ask", "--json", q], capture_output=True, text=True, check=True).stdout, q)
    n += 1
if platform.system() == "Linux" and os.path.exists("/sys/fs/cgroup/cpu.max"):
    for args in (["predict", "--json", "--cpu-quota", "1", "--cgroup", "/"],
                 ["predict", "--json", "--experimental", "--cpu-quota", "1", "--cgroup", "/"]):
        out = subprocess.run([osie] + args, capture_output=True, text=True).stdout
        check("predict", out, " ".join(args))
        n += 1
print(f"{n} outputs checked, {fails} schema violations")
sys.exit(1 if fails else 0)
