# Datasets

All intent data is produced by `ml/datasets/generate.py` (seed 13) plus one
hand-written file. Re-generate with:

```bash
python3 ml/datasets/generate.py                 # -> ml/datasets/out/{train,dev,test}.{jsonl,tsv}, report.json
```

## Label schema

Each example has an intent (22 classes, `OSIE_INTENTS` in
`include/osie/intent/intent.hpp`), BIO tags over the shared tokenizer
(`O, B/I-PROC, B/I-PID, B/I-REF, B/I-NUM`), and the gold frame fields used
for end-to-end scoring: `target_kind`, `target_value`, `concept`, `action`.

Gold-target conventions (identical to `build_frame()`):

* process intents (`process_cpu/memory/info/tree`), `action_request`,
  `container_throttle` → the named process, PID or reference; a process
  intent without one → `none` (ambiguous, OSIE asks a question);
* `help`, `unsupported`, `explain_concept`, `kubernetes` → `none`;
* everything else → `host`.

An example is **end-to-end correct** only if intent, target kind, target
value (case/space-insensitive), reference ordinal, concept (when given) and
action (when given) all match. A correct intent with a wrong process is a
failure, as required by the project brief.

## Splits and leakage controls

| Split | Source | Templates | Process names | Purpose |
|---|---|---|---|---|
| train | generator | 3+ families per intent | 82-name lexicon | fitting |
| dev | generator | held-out family (intents with ≥ 5 families) or random 10 % (others) | 16-name lexicon (disjoint) | model selection, threshold calibration |
| test | generator | last family of every intent (never in train/dev) | 52-name lexicon (disjoint) | reporting only |
| hand | `heldout_handwritten.tsv` | none — 210 utterances written independently, in free form | names absent from all lexicons (e.g. *Plex Media Server, Raycast, nsurlsessiond*) | reporting only |

* Paraphrase templates are grouped into **families**; whole families are held
  out, so no test template (or close variant) appears in training.
* Host phrases, concept phrases and off-topic prompts also have disjoint
  train/test lists.
* Noise (adjacent-letter swaps, deletions, case changes, dropped punctuation,
  fillers/suffixes) is applied only outside slot spans so tags stay correct.
* The hand-written set was written before any neural model was trained and
  is never used for selection or tuning. The rule parser was written before
  any dataset existed and was not tuned on these splits.

Caveat: dev is family-disjoint only for intents with ≥ 5 families; for the
others it shares families with train, which makes dev **optimistic** (measured:
dev ≈ 79 % vs. test ≈ 47 % E2E for the same neural model). Test and hand are
the numbers to trust.

## Sizes (seed 13)

| | examples | templates | families |
|---|---|---|---|
| train | 6 798 | — | — |
| dev | 810 | — | — |
| test | 1 020 | — | — |
| total generator | — | 356 | 91 |
| hand-written | 210 | — | — |

Multi-turn behaviour (references such as "it", "the second one", elliptical
"what about cpu?") is labelled at the single-utterance level (`reference`
targets with ordinals) and resolved deterministically by `SessionContext`;
dialogue-level behaviour is tested in `tests/unit/test_engine_session.cpp`.

## Data-generation cost accounting

* Compute: generation takes ~0.15 s on one core (`report.json: generation_seconds`).
* Human/teacher effort: the templates, lexicons and the 210 hand-written
  examples were authored **in-session by the LLM coding agent** that
  built this repository. No paid API was called by the pipeline, and no
  external dataset was used. The token cost of that authoring is part of the
  session and is not separately metered; it should be counted as
  "teacher-model assistance" when comparing total cost.
* Same-author bias: rules, templates and the hand-written set share an
  author, so the hand-written set measures robustness to *new phrasings and
  names*, not to a different population of users. Real user utterances are
  the main missing dataset.

## Optional generator flags (ablations)

* `--name-randomization P` — replace a share P of training process-name slots
  with synthetic names (`synthetic_name()`), forcing context-based tagging.
* `--max-train-families K` — keep only K training families per intent
  (diversity ablation; use `--per-template-train` to hold volume constant).

## CPU-contention telemetry (Milestone 5)

Produced by `ml/experiments/cpu_contention/run_lab.py run` from real Linux
containers; one JSON line per 500 ms window with telemetry features and the
probe's latency outcome. See `docs/RESEARCH.md` §5.
