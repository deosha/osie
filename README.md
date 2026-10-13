# OSIE — OS-Native Intelligence Engine

A small, local, **read-only** engine that answers questions about the
machine it runs on, such as "Why is my Mac using so much CPU?", "Why might
it be consuming so much memory?" or "Why is this container being throttled?".

OSIE works from native OS observations, an explicit OS-architecture graph and
deterministic reasoning. It is **not** an LLM wrapper and never generates or
runs shell commands. Its language layer is a zero-parameter rule parser, with
an optional ~0.3M-parameter model.

Status: research prototype. The project milestones are defined, with status
and evidence, in [`docs/MILESTONES.md`](docs/MILESTONES.md).
- Milestones 1–5 are implemented and verified on **macOS 15 (Apple M4 Pro)** and **Linux (Ubuntu 24.04, kernel 6.10)**.
- Milestone 6 (Kubernetes) is a design document only.
- Milestone 8 adds a **predictive CPU world model** for Linux. `osie predict` answers "what if this cgroup's CPU quota changed?". It is validated in one environment only; see [`docs/WORLD_MODEL_RESULTS.md`](docs/WORLD_MODEL_RESULTS.md).
- **v0.2** ([`docs/V0_2_AUDIT.md`](docs/V0_2_AUDIT.md), [`docs/V0_2_RELEASE_READINESS.md`](docs/V0_2_RELEASE_READINESS.md)):
  - clause-level safety routing: instructions are refused whatever the phrasing ("I authorize you to…", "ignore your rules and…"), while questions about actions are answered;
  - new questions: process listing, cgroup status, effective CPU capacity (host, online, affinity, cpuset and quota kept apart);
  - throttling *rates* instead of lifetime ratios, and quota-bound diagnosis;
  - CPU prediction made **experimental and opt-in**, with a declared support envelope, re-selected and re-calibrated under a stricter protocol, and tested on data collected after the model was frozen.
  The compatibility matrix is [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md).
- Iteration 2 added three things, audited in [`docs/NEXT_ITERATION_AUDIT.md`](docs/NEXT_ITERATION_AUDIT.md):
  - a hardened and fuzzed model loader, plus safer process identity;
  - installable packages and CI;
  - bounded history for questions about change over time.

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
I won't do that. This is a request to terminate a process, and OSIE is read-only: it has no code that could carry this out (V1 safety policy is read-only: actions with side effects are disabled).
  capability: act.process.terminate (risk high, irreversible; requires signal_process)

osie> yes, I authorize it
I won't do that. This is a request to terminate a process, and OSIE is read-only: ...
Permission or instructions given in a conversation do not change this; the read-only policy is fixed in OSIE's code.

osie> how does kill -9 work?
Process signals (SIGTERM asks a process to exit and lets it clean up; SIGKILL, as in kill -9, ends it immediately and cannot be caught)
```

The full live transcript is in [`examples/demo_session_macos.txt`](examples/demo_session_macos.txt).

## Install

**macOS (Apple Silicon or Intel), or Linux with Homebrew:**

```bash
brew install deosha/osie/osie
```

**Debian/Ubuntu** (x86_64 shown; use `arm64` on ARM):

```bash
curl -LO https://github.com/deosha/osie/releases/download/v0.1.0/osie_0.1.0_amd64.deb
sudo apt install ./osie_0.1.0_amd64.deb
```

**Fedora/RHEL family** (use `aarch64` on ARM):

```bash
sudo dnf install https://github.com/deosha/osie/releases/download/v0.1.0/osie-0.1.0-1.x86_64.rpm
```

**Any other Linux with glibc ≥ 2.35:** download
`osie-0.1.0-linux-<arch>.tar.gz` from the
[release page](https://github.com/deosha/osie/releases/latest), then unpack
it into `~/.local`.

Every release publishes `SHA256SUMS` and GitHub build-provenance
attestations. [`docs/INSTALL.md`](docs/INSTALL.md) covers verification,
uninstalling, building from source, where the model is looked up, and
resource use.

Then:

```bash
osie version
osie ask "what is using the most cpu?"
```

## Use

```bash
osie                                   # interactive session
osie --watch 5                         # ...and record CPU/memory/top processes every 5 s (in memory only)
osie ask "why is my mac slow"          # one question
osie ask --json "is memory pressure high?"
osie ask --watch 2 --for 60 "has cpu usage increased?"   # sample for a minute, then answer
osie parse "why is chrome using so much cpu"   # validated intent frame only (no observation)
osie snapshot | caps | graph [--dot --concepts] | version
osie --parser hybrid ...               # use the trained intent model instead of the rules
osie --audit audit.jsonl ...           # log every policy decision
```

On Linux, `osie predict --experimental --cpu-quota 2 --name api` (or, in a
session started with `OSIE_EXPERIMENTAL_PREDICT=1`, `predict cpu quota 2 for
api`) estimates the cgroup's CPU usage, throttling and CPU pressure under the
new quota. Prediction is **experimental and off by default**, and is given
only inside the model's validated envelope (`osie version` shows it):
- each estimate comes with calibrated 80% and 95% intervals;
- OSIE gives no numbers when the case is outside what the model was
  validated on;
- it never predicts application latency, which OSIE cannot observe;
- it never applies the change.

Questions about change over time are answered only from samples OSIE itself
recorded in the current session. With too little history OSIE says so, and
never guesses from one snapshot. Examples: "has CPU usage increased in the
last five minutes?", "which process has been growing in memory?", "when did
memory pressure become elevated?", "is the system recovering?".

If a name matches several processes, OSIE says how many matched and which
one it describes. For CPU and memory questions it also reports their
combined total. An earlier answer that is too old, or a PID now held by a
different process, makes OSIE ask again instead of assuming.

## Develop and test

Requirements:
- C++20 compiler (Apple clang 17 / GCC 12+ tested)
- CMake ≥ 3.24 and Ninja
- GoogleTest (optional: Google Benchmark)

On macOS: `brew install cmake ninja googletest google-benchmark`.

```bash
cmake -S . -B build -G Ninja && ninja -C build
./build/osie_unit_tests          # 237 tests (mock OS, deterministic)
./build/osie_platform_tests      # real-kernel integration tests
```

On Linux the build and commands are the same. Linux is tested in Docker:

```bash
docker build -t osie-linux-dev -f docker/linux-dev.Dockerfile docker/
docker run --rm -v "$PWD":/src:ro osie-linux-dev bash -c \
  'cmake -S /src -B /tmp/b -G Ninja && ninja -C /tmp/b && /tmp/b/osie_unit_tests && /tmp/b/osie_platform_tests'
```

CI (`.github/workflows/ci.yml`) runs:
- the tests on macOS arm64, Linux x86_64 and Linux arm64;
- ASan/UBSan and TSan;
- a package smoke test;
- a 5-minute libFuzzer run on the model loader;
- the C++/Python feature-parity check.

## What it does

| Area | Implemented |
|---|---|
| macOS adapter | Mach host APIs, libproc, sysctl, getfsstat, PF_ROUTE `NET_RT_IFLIST2`, NSProcessInfo (Objective-C++); CPU/topology (P/E clusters), memory + compressor + pressure level, processes (CPU, footprint, threads, ancestry), filesystems, interfaces, thermal state |
| Linux adapter | procfs, sysfs, cgroup v2, PSI, rtnetlink; plus systemd unit inferred from the cgroup path |
| Capabilities and safety | typed capability registry with validated descriptors; policy engine; audit log; **every action capability denied, with no executor code at all** |
| Knowledge graph | typed nodes/edges, enforced schema, provenance, temporal validity, platform sets, and edge nature (invariant / observed / inferred / hypothesized), with documentation references; 31-concept ontology; incremental instance updates |
| Reasoning | CPU, memory, slowness, per-process CPU/memory, concept explanations; findings typed as observation / hypothesis / correlation / causal explanation / investigation / proposal; missing evidence always listed |
| Conversation | follow-ups ("what about cpu?"), references ("it", "the second one") that expire after 10 min, (pid, start time) identity with PID-reuse detection, explicit handling of names that match several processes, observation freshness and reuse |
| Prediction (Linux) | read-only `osie predict`; CPU usage, throttling and PSI under a hypothetical quota or cpuset change. Uses a shared C++ feature encoder, a 16 KB model, conformal intervals, OOD abstention, and mechanisms from the graph. |
| Temporal | bounded in-memory history (no daemon, no disk); trend, onset, recovery and "which process is growing" from a Mann–Kendall test plus an effect-size floor; insufficient history reported, never guessed |
| Intent models | dataset generator with leakage controls; rules, TF-IDF, bag, CNN and BiGRU (17k–9.6M params), 3 seeds; dependency-free C++ inference with int8 option; hardened loader ([`docs/MODEL_FORMAT.md`](docs/MODEL_FORMAT.md)) fuzzed in CI |
| Distribution | install rules, `.tar.gz`/`.deb`/`.rpm` via CPack, glibc 2.35 baseline, release workflow with checksums and provenance, clean-environment smoke test |
| Experiments | isolated Linux CPU-contention study (`tools/cpu_lab.cpp`) |

## Key results (details in [`docs/RESEARCH.md`](docs/RESEARCH.md) and [`docs/WORLD_MODEL_RESULTS.md`](docs/WORLD_MODEL_RESULTS.md))

- **Predicting the effect of a CPU quota change: the mechanism beat learning outside the training workloads.** The setup was 752 randomised interventions in disposable Linux containers (one VM environment).
  - A zero-parameter model built on one measured kernel fact (demand ≈ usage + throttled CPU time) was best for usage and throttling on unseen workload families, quotas and workload classes. On batch jobs its error was 0.005 cores.
  - A 660-parameter learned hybrid won only on runs of the training families. On batch jobs it was off by 0.4 cores.
  - OSIE ships the mechanism plus a small learned model for CPU pressure. Prediction takes ~1 µs.
  - Application latency could not be predicted from OS telemetry on unseen workloads, so OSIE never predicts it.

- **Rules ≈ tiny models for language.**
  - On held-out sets the zero-parameter rule parser scores 65.5% (synthetic test) and 71.0% (hand-written) end-to-end.
  - The best tiny hybrid scores 67.6% / 73.0%. The dev-selected configuration averages 64.8% / 70.8% over 3 seeds; the shipped seed scores 63.2% / 69.5%.
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
tests/              unit (mock OS), platform (live kernel), Linux fixture /proc tree, fuzz (libFuzzer)
benchmarks/         Google Benchmark harness
ml/                 dataset generator, training, baselines, experiments, results
models/             trained .osiem models (deployed: intent.osiem)
docs/               INSTALL, MILESTONES, ARCHITECTURE, SECURITY, MODEL_FORMAT, DATASETS, BENCHMARKS, RESEARCH,
                    RESEARCH_ROADMAP, NEXT_ITERATION_AUDIT, PRIOR_ART, KUBERNETES
packaging/          release builder image, Linux package script, clean-environment smoke test
.github/workflows/  CI and release pipelines
```

## Limitations (honest list)

- **Read-only.** OSIE observes and explains; it never tunes. Predictions are never presented as observations.
- **No privileges.** It does not escalate. On macOS about 190 root-owned processes are only partly visible, and OSIE says so.
- **Predictions are experimental and opt-in.** The shipped model's support envelope is the one environment it was trained in (Linux 6.10, aarch64). Data collected after freezing, including other kernels and x86-64, are reported in [`docs/WORLD_MODEL_RESULTS.md`](docs/WORLD_MODEL_RESULTS.md), but the envelope was not widened on that basis. Workloads are synthetic; CPU-pressure predictions are marked experimental.
- **Short history only.** One 0.5 s sampling window per question. Trends need samples recorded in the same session (`--watch`); nothing is persisted between sessions.
- **macOS gaps.** No public API for temperatures or CPU frequency, or for mapping a PID to its launchd job label.
- **Templated answers.** Answers are deterministic templates, not open-ended conversation. The intent model (~0.3M parameters) classifies questions; it does not generate text.
- **Same-author intent data.** The v0.2 test set was frozen before the parser changes and split by family, but written by the same developer; real user utterances are still the main missing ingredient.
- **No Kubernetes support yet.** Kubernetes support is a design only ([`docs/KUBERNETES.md`](docs/KUBERNETES.md)).
- **Energy not measured.** It would require root.

Contributions are welcome; see [`CONTRIBUTING.md`](CONTRIBUTING.md).

Licensed under the [Apache License 2.0](LICENSE).
