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
