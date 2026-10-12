# Milestones

The project plan has six milestones. This page defines each one, states its
status, and points to the evidence. "Done" means implemented *and* verified by
tests or measurements in this repository. Anything not done is listed explicitly.

| # | Milestone | Status |
|---|---|---|
| 1 | Native macOS engine | Done |
| 2 | Tiny neural understanding | Done (the model is not the default; see below) |
| 3 | Cross-platform Linux support | Done |
| 4 | OS reasoning | Done, with documented limitations |
| 5 | Learned predictive-optimization research | Done (diagnosis only; no tuning) |
| 6 | Future Kubernetes design | Design only, not implemented |
| 7 | Iteration 2: hardening, distribution, temporal state | Done, except the items below |
| 8 | Predictive CPU world model (v0.2) | Done in one environment; hypotheses partly supported (see below) |

## 1. Native macOS engine

**Goal:** a C++20 application that answers questions about a Mac from real
native observations, without an LLM.

| Deliverable | Where |
|---|---|
| Compiling C++20 application | `CMakeLists.txt`, `src/cli/main.cpp` |
| Native macOS adapter | `src/platform/macos/` (Mach host APIs, libproc, sysctl, getfsstat, routing sysctl, NSProcessInfo via Objective-C++) |
| Real CPU and memory observations | `cpu_memory.cpp`; `tests/platform/test_native.cpp` |
| Process enumeration | `process.cpp`; lifecycle-race test with a forked child |
| Typed capability registry | `include/osie/capabilities/capability.hpp`; `tests/unit/test_capabilities.cpp` |
| Basic OS architecture graph | `include/osie/architecture/`; `tests/unit/test_graph.cpp` |
| Deterministic intent parser | `src/intent/rule_parser.cpp`; `tests/unit/test_intent.cpp` |
| Interactive CLI | `osie` REPL; `examples/demo_session_macos.txt` |
| Unit and integration tests | `tests/unit/` (mock OS), `tests/platform/` (live kernel) |

## 2. Tiny neural understanding

**Goal:** a trained compact intent model, integrated into C++, compared with
non-neural baselines, with measured costs.

| Deliverable | Where |
|---|---|
| Reproducible intent dataset | `ml/datasets/generate.py`, `ml/datasets/heldout_handwritten.tsv`; `docs/DATASETS.md` |
| Trained tiny neural models | `models/*.osiem` (bag, CNN, BiGRU; f32 and int8) |
| Classical and deterministic baselines | rule parser; `ml/training/baseline_tfidf.py` |
| Local inference in C++ | `src/inference/neural_parser.cpp` (`--parser hybrid`) |
| Model evaluation | `tools/osie_eval.cpp`, `ml/experiments/run_all.py`; `docs/RESEARCH.md` §2 |
| Training-cost measurements | `docs/BENCHMARKS.md` (time, estimated FLOPs, peak RSS, artifact size) |
| Inference benchmarks | `benchmarks/bench_main.cpp`, `tools/ort_bench.cpp` |

**Outcome:** no trained model beat the zero-parameter rules by more than seed
variance on held-out data, so the CLI defaults to rules. Energy was not measured.

## 3. Cross-platform Linux support

**Goal:** genuinely native Linux adapters, tested on Linux (not only cross-compiled).

| Deliverable | Where |
|---|---|
| Linux-native adapters | `src/platform/linux/` (procfs, sysfs, rtnetlink) |
| Process, CPU and memory observations | `process.cpp`, `cpu_memory.cpp` |
| cgroup v2 metrics | `cgroups.cpp` |
| PSI metrics | `cgroups.cpp` (`/proc/pressure/*`, per-cgroup `*.pressure`) |
| Platform capability differences | `IMacExtensions` / `ILinuxExtensions` in `include/osie/platform/os.hpp` |
| Linux integration tests | `tests/platform/test_native.cpp`, `tests/platform/test_linux_parsers.cpp` (fixture `/proc` tree); run in `docker/linux-dev.Dockerfile` |

Not covered: systemd D-Bus (service ownership is inferred from the cgroup
path instead) and eBPF.

## 4. OS reasoning

**Goal:** evidence-backed diagnoses that separate observation from hypothesis.

| Deliverable | Where |
|---|---|
| CPU diagnostic reasoning | `Reasoner::cpu` in `src/reasoning/reasoner.cpp` |
| Memory-pressure diagnosis | `Reasoner::memory` |
| Architecture graph queries | `KnowledgeGraph::path`, `consequences`, `check_consistency` |
| Evidence-backed explanations | typed findings with evidence ids and ontology mechanisms |
| Multi-turn follow-ups | `SessionContext` in `src/engine/engine.cpp`; `tests/unit/test_engine_session.cpp` |
| Known limitations and unsupported cases | `docs/RESEARCH.md` §4, `README.md` |

## 5. Learned predictive-optimization research

**Goal:** a safely isolated experiment on CPU contention comparing tiny
predictive models with rules. No autonomous tuning.

| Deliverable | Where |
|---|---|
| Isolated CPU-contention scenarios | `tools/cpu_lab.cpp`, `ml/experiments/cpu_contention/run_lab.py` (disposable, resource-limited containers) |
| Structured feature extraction | per-window telemetry from OSIE's Linux adapter |
| Tiny predictive models vs rules | logistic regression, depth-3 tree, small MLP vs a-priori rules |
| Reproducible results | `ml/experiments/results/cpu_contention.json`; `docs/RESEARCH.md` §5 |

Autonomous tuning is deliberately not implemented.

## 6. Future Kubernetes design

**Goal:** specify Kubernetes integration without claiming it exists.

The specification is in `docs/KUBERNETES.md`: typed integration, the resource
relationship graph, pod → container → cgroup mapping, CPU/memory analysis, a
safe recommendation architecture, and RBAC/admission considerations.

**Not implemented.** OSIE recognises Kubernetes questions and says so.

## 7. Iteration 2: hardening, distribution, temporal state

**Goal:** make OSIE reliable on untrusted input, installable without a source
checkout, and able to answer questions about change over time. The
evidence-backed audit, with file and line references for every finding, is
in [`NEXT_ITERATION_AUDIT.md`](NEXT_ITERATION_AUDIT.md).

| Deliverable | Where |
|---|---|
| Hardened, specified model format | `src/inference/neural_parser.cpp`; [`MODEL_FORMAT.md`](MODEL_FORMAT.md); `InferenceHardening.*` |
| Fuzzing | `tests/fuzz/`; CI `fuzz` job; `BENCHMARKS.md` (*Fuzzing*) |
| Process identity and ambiguity, context expiry | `SessionContext::resolve`; `Identity.*` |
| Safety tests: adversarial phrasing, multi-turn, malformed parser output | `tests/unit/test_identity_safety.cpp` |
| Model discovery independent of the source tree | `include/osie/inference/model_locator.hpp`; `ModelLocator.*` |
| Packages and release pipeline | `CMakeLists.txt` (install, CPack), `packaging/`, `.github/workflows/`; [`INSTALL.md`](INSTALL.md); release v0.1.0; Homebrew tap `deosha/homebrew-osie` |
| Bounded history and temporal questions | `include/osie/state/history.hpp`; `Temporal*`, `Trend.*`, `History.*` |

Not done in this iteration (see [`RESEARCH_ROADMAP.md`](RESEARCH_ROADMAP.md)):
- persistent history;
- a response-generator interface;
- the OS world-model experiments;
- Kubernetes integration.

## 8. Predictive CPU world model (v0.2)

**Goal:** a compact model that predicts how a cgroup's CPU state changes
under a hypothetical intervention. It must be trained and evaluated on real,
randomised interventions in disposable Linux environments, and integrated
read-only into OSIE.

| Acceptance criterion | Evidence |
|---|---|
| Controlled interventions in disposable Linux environments | `ml/world_model/collect.py`: 64 container runs, 752 episodes, quota/cpuset applied with `docker update` from outside |
| True pre/post observations | PRE/POST windows per episode; requested vs applied values recorded |
| Reproducible baselines | no-change and mechanistic in `ml/world_model/experiments.py` |
| Tiny trained models | ridge, GBT, MLPs (388–164k params), GRUs, residual hybrids |
| Held-out runs | run-level splits, a held-out family, held-out quotas, counterfactual runs and a held-out workload class |
| Error and uncertainty measured | run-bootstrap CIs, split-conformal coverage, OOD flag rates |
| Ablation of architecture-aware features | Experiments 1, 3, 5 and 8 in [`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md) |
| Total training cost documented | *Cost* section of the results (collection dominates) |
| Model loadable from C++ | `predict::WorldModel`; parity with Python ≤ 1.4e-7 |
| OSIE answers a supported hypothetical | `osie predict --cpu-quota`; checked live against a real kernel |
| Explicit abstention | out-of-range actions and features, missing telemetry, short history; `WorldModelPredict.AbstainsOutsideValidatedScope` |
| No prediction can execute an operation | `observe.*` capability; `EnginePredict.AnswersReadOnlyAndAudits` |
| Existing tests still pass | 137 unit tests plus platform tests |
| Claims backed by measurements | [`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md) |

**Outcome:**
- Learned models beat the baselines only in-distribution.
- The shipped predictor is the mechanistic model, plus a learned residual for
  CPU pressure only.
- Experiment 7 (cross-hardware) was not possible with one environment.
