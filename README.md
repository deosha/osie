# OSIE — OS-Native Intelligence Engine

A small, local, **read-only** engine that answers questions about the
machine it runs on, such as "Why is my Mac using so much CPU?", "Why might
it be consuming so much memory?" or "Why is this container being throttled?".

OSIE works from native OS observations, an explicit OS-architecture graph and
deterministic reasoning. It is **not** an LLM wrapper and never generates or
runs shell commands. Its language layer is a zero-parameter rule parser, with
an optional ~0.3M-parameter model.

Status: research prototype.
- Milestones 1–5 are implemented and verified on **macOS 15 (Apple M4 Pro)** and **Linux (Ubuntu 24.04, kernel 6.10)**.
- Milestone 6 (Kubernetes) is a design document only.

```text
$ osie
osie> What is using CPU?
Processes using the most CPU (over the last 0.5 s; 100% = one full core):
  1. Code Helper (Renderer) (pid 39758)  41.8%  23 threads
  2. Webex (pid 654)  14.4%  14 threads
  ...
Note: 188 processes of other users (root daemons) are not readable without privileges and are not ranked.

osie> Which one is using the most memory?
  1. com.apple.Virtualization.VirtualMachine (pid 27353)  7.9 GiB  (33.0% of RAM)
System memory pressure: warning.

osie> Why might it be consuming so much memory?
com.apple.Virtualization.VirtualMachine (pid 27353) is using 7.9 GiB (33.0% of RAM).
Evidence (observed):
  - footprint 7.9 GiB vs resident 2.6 GiB
  - kernel memory-pressure level: warning
  - compressor holds 9.7 GiB (40.5% of RAM)
Analysis:
  [Observed, high] This is the process's physical footprint ... virtual size ... is not a measure of RAM use.
  [Hypothesis, low] A memory leak is possible only if usage keeps growing ...; one sample cannot show that.
  [Correlation, limited] The system shows memory pressure and this process holds 33.0% of RAM ...
      mechanism: Memory pressure -AFFECTS-> Paging (page-outs/page-ins) (invariant)
Not observed / uncertain:
  - memory growth over time (needs repeated samples)
Certainty: limited.

osie> kill it
I won't do that: V1 safety policy is read-only: actions with side effects are disabled.
  capability: act.process.terminate (risk high, irreversible; requires signal_process)
```

The full live transcript is in [`examples/demo_session_macos.txt`](examples/demo_session_macos.txt).

## Build and run

Requirements:
- C++20 compiler (Apple clang 17 / GCC 13 tested)
- CMake ≥ 3.24 and Ninja
- GoogleTest (optional: Google Benchmark)

On macOS: `brew install cmake ninja googletest google-benchmark`.

```bash
cmake -S . -B build -G Ninja && ninja -C build
./build/osie                                   # interactive session
./build/osie ask "why is my mac slow"          # one question
./build/osie ask --json "is memory pressure high?"
./build/osie parse "why is chrome using so much cpu"   # validated intent frame only
./build/osie snapshot | caps | graph [--dot --concepts]
./build/osie --parser hybrid ...               # use the trained model (models/intent.osiem)
./build/osie --audit audit.jsonl ...           # log every policy decision
```

Run the tests:

```bash
./build/osie_unit_tests          # 80 tests (mock OS, deterministic)
./build/osie_platform_tests      # real-kernel integration tests
```

On Linux the build and commands are the same. The repository was tested in
Docker:

```bash
docker build -t osie-linux-dev -f docker/linux-dev.Dockerfile docker/
docker run --rm -v "$PWD":/src:ro osie-linux-dev bash -c \
  'cmake -S /src -B /tmp/b -G Ninja && ninja -C /tmp/b && /tmp/b/osie_unit_tests && /tmp/b/osie_platform_tests'
```

## What it does

| Area | Implemented |
|---|---|
| macOS adapter | Mach host APIs, libproc, sysctl, getfsstat, PF_ROUTE `NET_RT_IFLIST2`, NSProcessInfo (Objective-C++); CPU/topology (P/E clusters), memory + compressor + pressure level, processes (CPU, footprint, threads, ancestry), filesystems, interfaces, thermal state |
| Linux adapter | procfs, sysfs, cgroup v2, PSI, rtnetlink; plus systemd unit inferred from the cgroup path |
| Capabilities and safety | typed capability registry with validated descriptors; policy engine; audit log; **every action capability denied, with no executor code at all** |
| Knowledge graph | typed nodes/edges, enforced schema, provenance, temporal validity, platform sets, and edge nature (invariant / observed / inferred / hypothesized), with documentation references; 31-concept ontology; incremental instance updates |
| Reasoning | CPU, memory, slowness, per-process CPU/memory, concept explanations; findings typed as observation / hypothesis / correlation / causal explanation / investigation / proposal; missing evidence always listed |
| Conversation | follow-ups ("what about cpu?"), references ("it", "the second one"), PID-reuse-safe identity, observation freshness and reuse |
| Intent models | dataset generator with leakage controls; rules, TF-IDF, bag, CNN and BiGRU (17k–9.6M params), 3 seeds; dependency-free C++ inference with int8 option |
| Experiments | isolated Linux CPU-contention study (`tools/cpu_lab.cpp`) |

## Key results (details in [`docs/RESEARCH.md`](docs/RESEARCH.md))

- **Rules ≈ tiny models for language.**
  - On held-out sets the zero-parameter rule parser scores 65.5% (synthetic test) and 71.0% (hand-written) end-to-end.
  - The best tiny hybrid scores 67.6% / 73.0%; the dev-selected model 64.8% / 70.8%.
  - Above ~0.3M parameters, scale did not help. Template **diversity** did: 36.7% → 47.9% at equal data volume.
  - **Rules are the default**; the model is opt-in.
- **Knowledge beats generic learning for diagnosis.** On held-out container workloads:
  - Rules from documented kernel semantics (cgroup `cpu.stat`, PSI) reach 91.7%.
  - Learned models on all telemetry reach 67%: they learned a load-average confound.
  - The same models on mechanistic features reach 100%.
  - Quota throttling inflated latency 6–15×; same-CPU contention barely affected the probe.
- **Cheap to run.**
  - A question costs ~40 ms of work on top of a 500 ms sampling window, with 8–11 MB RSS.
  - Neural parsing takes 6–36 µs.
  - The custom inference path is 1–6× faster than ONNX Runtime and uses ~6× less memory, matching it to 1e-6.

## Repository map

```text
include/osie/       public headers: core, platform, capabilities, state, architecture, reasoning, intent, inference, engine
src/                implementation; src/platform/{macos,linux}
tools/              osie_eval (end-to-end evaluation), osie_ort_bench, osie_cpu_lab (Linux)
tests/              unit (mock OS), platform (live kernel), Linux fixture /proc tree
benchmarks/         Google Benchmark harness
ml/                 dataset generator, training, baselines, experiments, results
models/             trained .osiem models (deployed: intent.osiem)
docs/               ARCHITECTURE, SECURITY, DATASETS, BENCHMARKS, RESEARCH, PRIOR_ART, KUBERNETES
```

## Limitations (honest list)

- **Read-only.** OSIE observes and explains; it never tunes. Predictions are never presented as observations.
- **No privileges.** It does not escalate. On macOS about 190 root-owned processes are only partly visible, and OSIE says so.
- **Short snapshots.** One 0.5 s sampling window per question; no history or trends yet.
- **macOS gaps.** No public API for temperatures or CPU frequency, or for mapping a PID to its launchd job label.
- **Templated answers.** Answers are deterministic templates, not open-ended conversation.
- **Synthetic intent data.** The intent data is synthetic and written by the same author as the rules; real user utterances are the main missing ingredient.
- **No Kubernetes support yet.** Kubernetes support is a design only ([`docs/KUBERNETES.md`](docs/KUBERNETES.md)).
- **Energy not measured.** It would require root.

License: Apache-2.0 (intended; add a LICENSE file before publishing).
