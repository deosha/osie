# OSIE research report

**Question.** How little neural computation does useful OS understanding need,
and can tiny models beat deterministic, knowledge-based methods?

**Short answer, from this prototype's evidence:**

1. **Language understanding.** A rule parser with zero parameters, built on
   an explicit intent schema, was *not* reliably beaten by any trained model
   on held-out data:
   - Models tested: 17k–9.6M parameters, bag, CNN and BiGRU, with and without knowledge features.
   - The best tiny hybrids (110k–840k parameters with a calibrated rule fallback) gain about 2 points over rules, roughly within seed variance.
   - The dev-selected model *ties* the rules.
   - Above ~0.3M parameters, scale bought nothing on held-out phrasing.

   The binding constraint is **data diversity, not model capacity.**
2. **OS diagnosis (Linux CPU bottlenecks).** In controlled, isolated experiments:
   - Rules written in advance from documented kernel semantics (cgroup `cpu.stat`, PSI) reached 91.7% accuracy on held-out workloads.
   - Learned models given *all* telemetry fell to 67%, because they learned a temporal confound.
   - The same models given only *mechanistic* features reached 100%.

   Structured systems knowledge did the heavy lifting; learning was useful only for fitting thresholds.
3. **Inference.** A specialised ~300-line C++ forward pass is 1–6× faster
   than ONNX Runtime, loads 4–8× faster and uses ~6× less memory. It matches
   numerically to 1e-6 and agrees with ORT on every prediction tested.

Everything below is measured. Where something was not measured, the text says so.

---

## 1. Setup

- **Code:** C++20 engine (`src/`), Python training (`ml/`).
- **Machine:** Apple M4 Pro, macOS 15.7.3; Linux experiments run in Docker (kernel 6.10). See `docs/BENCHMARKS.md`.
- **Dataset:** `docs/DATASETS.md`:
  - 6,798 train / 810 dev / 1,020 test synthetic examples. Test is **template-family- and name-lexicon-disjoint** from train.
  - A separate **210-utterance hand-written** set, written before any model was trained.
- **Scoring:** a frame is correct only if intent, target (process name, PID or reference ordinal), concept and action all match. A correct intent with the wrong process counts as a failure. All parsers go through the same C++ frame builder (`tools/osie_eval`).
- **Protocol:** 3 seeds per configuration. Selection and threshold calibration use dev only. Test and hand-written sets are for reporting only.

## 2. Intent understanding: results

Columns are end-to-end accuracy (%), mean ± std over 3 seeds.

- **Hybrid** = neural first; falls back to rules below a confidence threshold calibrated on dev.
- **dev** is optimistic (see §2.3). **test** and **hand** are the numbers to trust.

| Parser | Params | dev | test | hand | Latency (C++) |
|---|---|---|---|---|---|
| **Rules** | 0 | 73.1 | **65.5** | **71.0** | 1 µs |
| TF-IDF + logistic regression (rule slots) | 211k | 76.2 | 55.8 | 71.9 | 430 µs (Python) |
| bag 20k, neural only | 17.5k | 62.0 ± 1.5 | 31.7 ± 0.2 | 39.5 ± 1.4 | 4 µs |
| bag 300k, neural only | 266k | 79.3 ± 1.0 | 47.2 ± 3.5 | 52.2 ± 2.5 | 6 µs |
| CNN 300k, neural only | 276k | 73.7 ± 4.4 | 45.0 ± 1.8 | 52.1 ± 3.0 | 35 µs |
| BiGRU 300k, neural only | 291k | 78.1 ± 1.7 | 47.5 ± 2.3 | 54.9 ± 2.5 | 33 µs |
| BiGRU 9.6M, neural only | 9.6M | 75.1 ± 2.1 | 52.5 ± 3.9 | 59.4 ± 1.8 | 224 µs |
| bag 20k, hybrid | 17.5k | 77.0 ± 2.1 | 58.0 ± 1.2 | 61.0 ± 2.3 | 4 µs |
| BiGRU 110k, hybrid | 113k | 81.9 ± 0.8 | 67.5 ± 0.7 | **73.0 ± 1.0** | 19 µs |
| BiGRU 300k, hybrid | 291k | 84.0 ± 1.7 | 63.1 ± 5.2 | 70.3 ± 3.5 | 33 µs |
| BiGRU 840k, hybrid | 837k | 82.7 ± 1.5 | **67.6 ± 0.8** | 72.2 ± 1.6 | 59 µs |
| BiGRU 2.9M, hybrid | 2.9M | 82.1 ± 1.0 | 65.5 ± 1.2 | 72.2 ± 2.1 | 119 µs |
| BiGRU 9.6M, hybrid | 9.6M | 84.1 ± 0.8 | 67.5 ± 1.1 | 72.7 ± 3.0 | 224 µs |
| bag 300k + knowledge features, hybrid (**dev-selected, shipped**) | 266k | **86.3 ± 1.7** | 64.8 ± 3.6 | 70.8 ± 1.2 | 7 µs |
| BiGRU 840k + name randomisation, hybrid | 837k | 81.9 | 64.9 ± 2.2 | 73.0 ± 0.6 | 59 µs |

The full results, including every variant, seed and per-intent breakdown,
are in `ml/experiments/results/summary.json` and `ablation_names.json`.

### 2.1 Findings

- **Rules are a strong baseline.**
  - With zero parameters and 1 µs per utterance, the rules are within about two points of the best hybrid.
  - Only BiGRU 110k/840k hybrids exceed them on both held-out sets, by 1.5–2 points, which is comparable to seed standard deviation.
  - **No model is clearly necessary.** OSIE therefore ships with the rule parser as default, and the hybrid model is opt-in (`--parser hybrid`).
- **Neural-only models overfit templates.**
  - Template-disjoint test accuracy (31–53%) is far below dev (62–80%).
  - Error analysis on the hand-written set shows two causes:
    - **Slot tagging memorised the name lexicon.** Unseen names such as "Plex Media Server" get no target, and "90 percent" is tagged as a PID.
    - **Intent families never seen in training are misclassified.** For example, every `cpu_topology` test template is classified as `top_cpu`.
- **Scale does not help past ~0.3M parameters.**
  - Neural-only accuracy improves only from ~47% to ~52% on test (and 55% → 59% on hand) going from 0.3M to 9.6M parameters.
  - Calibrated hybrids are flat at 65.5–67.6% on test.
  - Meanwhile latency grows ~7× (33 → 224 µs), artifact size ~33× (1.1 → 37 MB), and estimated training FLOPs ~4.6×.
- **The knowledge-feature effect is small.**
  - Adding OS-lexicon category features (`k:cpu`, `k:why`, `k:act`, …) gave the best dev score.
  - It changed held-out scores within noise: bag 300k hybrid on hand 68.3 → 70.8, test 62.3 → 64.8.
- **Name randomisation did not measurably help.**
  - Hand-written E2E for BiGRU 840k moved 72.2 → 73.0.
  - Target (slot) accuracy changed within seed noise: 840k test 67.0 → 67.9; 300k test 66.2 → 63.5 and hand 69.0 → 70.2.
  - The slot failures on unseen names are therefore *not* fixed by this technique at this data scale.
- **Int8 embedding quantisation is free.**
  - Dev accuracy is identical for every configuration (e.g. 80.5 vs 80.5), and files are 3.4× smaller.
  - Latency is unchanged, since dequantisation is cheap.
- **Classical TF-IDF** is the best on the hand-written set (71.9%) and the worst on the synthetic test (55.8%), at 430 µs in Python.

### 2.2 Sample efficiency and diversity

**Step-equalised learning curve** for bag 300k + knowledge features. Values are end-to-end %; the hybrid uses an uncalibrated threshold of 0.55.

| Training examples | dev (neural) | test (neural) | hand (neural) | test (hybrid) | hand (hybrid) |
|---|---|---|---|---|---|
| 339 (5%) | 59.5 | 40.3 | 41.9 | 53.4 | 57.3 |
| 679 (10%) | 67.8 | 45.2 | 48.1 | 56.8 | 64.4 |
| 1,699 (25%) | 76.4 | 48.2 | 52.4 | 56.8 | 66.8 |
| 3,399 (50%) | 77.8 | 45.8 | 54.0 | 53.7 | 64.9 |
| 6,798 (100%) | 80.5 | 49.3 | 55.2 | 60.2 | 66.5 |

Held-out accuracy saturates around **1–2k examples**; past that, more
examples from the *same* templates barely help.

A first version of this curve used a fixed epoch budget and collapsed at
5–10% of the data (dev ≈ 6%). That was an optimisation artifact, too few
gradient steps, not a property of the data. It is kept in `summary.json` and
labelled as such.

**Template diversity at constant volume (~7k examples):**

| Families per intent | dev | test |
|---|---|---|
| 1 | 46.4 ± 6.3 | 36.7 ± 3.7 |
| 2 | 68.1 ± 0.6 | 44.5 ± 4.0 |
| all (3–5) | 78.0 ± 1.1 | 47.9 ± 1.0 |

Diversity, not volume, drives generalisation. The cheapest next step is
therefore more varied (ideally real) utterances, not bigger models.

### 2.3 Threats to validity

- **dev is family-disjoint only for intents with ≥ 5 families,** which makes it optimistic: ~80% on dev vs ~50% on test for the same model.
- **Same-author bias.** One author (the coding agent) wrote the rules, the templates and the hand-written set. Real user utterances were not available.
- **Dataset size.** 210 hand-written utterances give a ±3-point 95% confidence interval at ~70% accuracy. Differences under ~4 points between parsers should not be over-read.
- **Parser scope.** Concept and action extraction are deterministic lexicons for every parser, so they do not differentiate the parsers.

## 3. Inference engineering

- **Model and file:** the `.osiem` format is a text header plus raw tensors. The model is an embedding bag over hashed character n-grams (FNV-1a) followed by a bag, CNN or BiGRU encoder, with an intent head and a BIO slot head.
- **Shared features:** tokenisation and hashing are byte-identical between Python and C++ (`ml/evaluation/check_parity.py`: 207 texts, 0 mismatches, including Unicode and knowledge features).
- **Numerical parity:** every trained model is checked against PyTorch, with max |Δp| ≤ 1.6e-6 and 100% argmax agreement.
- **vs ONNX Runtime:** results are in `docs/BENCHMARKS.md`. The custom path wins on latency (5.4 vs 32 µs for bag; 31 vs 45 µs for BiGRU), load time and memory. This is why ONNX Runtime is used only for benchmarking.
- **Not investigated:** SIMD, Accelerate and Metal. At 6–60 µs per utterance, inference is already ~100× cheaper than the observations it gates.

## 4. OS reasoning (Milestones 1, 3 and 4)

The deterministic reasoner runs on real macOS and Linux observations and
separates **observation, hypothesis, correlation, causal explanation,
suggested investigation and proposed optimisation**. Each hypothesis cites an
ontology edge, with its nature and documentation reference.

Behaviours were validated on live systems, and several rules exist because
live data broke earlier versions:

- **Unreadable processes are reported, not hidden.** On this Mac, 188–191 root-owned processes are only partly readable. OSIE reports the unattributable share of CPU instead of hiding it.
- **The macOS memory-pressure level is used directly.** The kernel level was "warning" during development, with a 9.7 GiB compressor and 14.5 GiB of swap in use. OSIE correctly tied that pressure to the Docker VM's 7.9 GiB footprint, as a *correlation*, not a cause.
- **Linux load average is not treated as current demand.** It lags by about a minute, so a high 1-minute load with few runnable tasks is reported as "demand was recent". Fixed after a live run.
- **Linux major faults alone are not memory pressure.** Cold program reads also cause them; they need swap-in corroboration. Fixed after a live run.

Known limitations:

- Single-window sampling; there are no trends.
- No per-thread CPU breakdown.
- No GPU, network latency or per-process network attribution.
- macOS exposes no temperature/frequency API and no PID → launchd job mapping.
- Thresholds are heuristics, listed in `reason::Thresholds`.

## 5. Learned CPU-bottleneck diagnosis (Milestone 5)

### Design

`tools/cpu_lab.cpp` runs a 10 ms periodic probe doing fixed work (~5 ms).
Meanwhile, OSIE's own Linux adapter samples:

- `/proc/stat`;
- system PSI;
- the probe container's own cgroup `cpu.stat` and `cpu.pressure`.

Each 500 ms window yields features plus the probe's p50/p95/p99 latency.

Scenarios run as 20 disposable containers on the Docker VM, each pinned to
2 vCPUs with 256 MB memory, no network and ~8 s lifetime:

- none;
- busy on *other* CPUs (label: none);
- CPU contention on the *same* cpuset;
- cgroup quota throttling;
- fsync-heavy I/O.

**Held-out test runs use workloads and parameters never seen in training:**

- a hash workload instead of spin;
- a duty-cycled load with 6 threads;
- a 0.8-CPU quota with a duty-cycled load;
- 4 I/O writers.

There are 196 training and 84 test windows.

### Results on held-out workloads

| Diagnoser | Parameters | Accuracy | Macro-F1 | Notes |
|---|---|---|---|---|
| Host CPU% and load (what a host monitor sees) | 3 thresholds | 48.8% | 0.38 | misses all contention and throttling inside containers |
| **A-priori rules** (throttled > 10% of periods; cgroup PSI > 5%; I/O PSI > 10%) | 3 thresholds | **91.7%** | **0.94** | contention recall 0.75: the duty-cycled load produced < 5% cgroup PSI |
| Logistic regression, all 18 features | 76 | 66.7% | 0.67 | |
| Depth-3 decision tree, all features | 7 nodes | 66.7% | 0.62 | root split on `load1` |
| MLP (16 hidden units), all features | 372 | 67.9% | 0.69 | abstains on 32% of windows; 98% correct where it answers |
| Logistic regression, mechanistic features | 36 | **100%** | 1.00 | |
| Depth-3 tree, mechanistic features | 7 nodes | **100%** | 1.00 | `cg_psi_cpu` → `cg_throttled_ratio` → `psi_io` |
| MLP (16), mechanistic features | 212 | **100%** | 1.00 | |

"Mechanistic features" means PSI (CPU, memory, I/O) and cgroup counters only.

Models trained in ≤ 0.11 s, and Python prediction takes 15–40 µs. A depth-3
tree is a handful of comparisons, so a C++ port would be trivial.

### Interpretation

- **Why the all-feature models failed.** They latched onto the host load average, which is a **confound**: the 1-minute load average carries over between sequentially run scenarios.
- **Knowledge supplied the right inputs.** Choosing features by OS mechanism (what the kernel documents as *measuring* contention or throttling) removed the confound. The learned tree then *rediscovered the rule structure* with a better contention threshold.
- **This is the project's central result in miniature:** knowledge chooses the signals, and a few learned parameters tune thresholds.

### Cause is not impact

Average p95 probe latency by scenario:

| Scenario | p95 latency |
|---|---|
| none | 5.3–5.9 ms |
| same-CPU contention | 4.9–6.1 ms |
| throttling, 0.8-CPU quota | 32 ms |
| throttling, 0.5-CPU quota | 60 ms |
| throttling, 0.3-CPU quota | 83 ms |

- **Contention barely hurt this periodic, mostly-sleeping task.** This is consistent with the Linux scheduler favouring tasks that wake from sleep, but this experiment does not prove that mechanism.
- **Quota throttling caused 6–15× latency inflation.**
- **"Bottleneck present" predicted felt degradation poorly.** Rules calling any bottleneck matched degradation only 58% of the time. A logistic-regression degradation predictor scored 100%, essentially by detecting throttling.
- **Diagnosing a cause and predicting user impact are different tasks.** They need different labels, and a predicted cause is not evidence that an intervention is safe.
- **Curiosity:** runs with background load *elsewhere* had *lower* probe latency (3.6–4.4 ms) than idle runs. A plausible explanation is vCPU idle-state wake-up cost in the VM, but this was not tested.

### Caveats

- One VM and one kernel (6.10 linuxkit on Apple Silicon virtualisation).
- 20 runs of 14 windows each; windows within a run are correlated.
- Labels are the injected scenario, not ground-truth causal measurements.
- 100% on 84 windows from 6 held-out runs is encouraging, not conclusive.
- Not tested: memory-bandwidth contention, SMT siblings, real multi-tenant traffic, and bare-metal hosts.

## 6. Cost accounting

### Training compute (per configuration, CPU only)

| Model | Training time | Estimated training FLOPs |
|---|---|---|
| Intent models, 17k–9.6M params | 5–23 s | 4–538 GFLOP |
| CPU diagnosers | < 0.2 s | — |

### Whole pipeline

| Stage | Wall time |
|---|---|
| Intent dataset generation | 0.15 s |
| Intent experiment suite (`run_all.py`) | 9.1 min |
| Name ablation + learning curve | ~8 min |
| CPU-lab data collection | 3 min 07 s |

### Teacher-model assistance

The templates, lexicons, rules and hand-written evaluation set were written
by the LLM coding agent during development. Its token cost is not metered by
the pipeline. It is the dominant "data" cost, and it should be counted when
comparing against approaches that fine-tune or call large models.

### Not measured

- Energy: `powermetrics` needs root, which OSIE never requests.
- GPU: not used.

## 7. What to do next, by evidence

1. **Collect real, consented utterances.** Diversity is the bottleneck (§2.2). A few hundred real queries would likely outweigh any architecture change.
2. **Keep the language layer at ≤ 0.3M parameters, or use rules.** Invest in slot generalisation: context-only tagging, and gazetteer lookup against the live process list, which is *knowledge-assisted inference* OSIE can do cheaply at runtime.
3. **Extend the CPU-lab study:**
   - more runs and hosts;
   - interleaved scenario order to break the load-average confound;
   - memory-bandwidth and SMT contention;
   - per-window trend features.

   Then port the mechanistic tree or logistic regression to C++ as an *advisory* signal with abstention.
4. **Keep prediction separate from action.** Milestone 5 shows that predicted causes and felt impact diverge; any future optimisation needs outcome-labelled evaluation and the safety architecture in `docs/SECURITY.md`.

## 8. Prior art

See `docs/PRIOR_ART.md`: bpftune, sched_ext/scx, schedcp, TuneD, k8sgpt and HolmesGPT.
