# Research roadmap

This plan covers the experimental directions that were **not** implemented in
this iteration. For each it states what would be built, how it would be
evaluated, and what result would justify adopting it. The engineering audit
is in [`NEXT_ITERATION_AUDIT.md`](NEXT_ITERATION_AUDIT.md). The existing
results are in [`RESEARCH.md`](RESEARCH.md).

Central question, unchanged: *can structured OS knowledge plus very efficient
learning give useful OS intelligence at a small fraction of the cost of a
conventional language model?*

The evidence so far:
- For language understanding, ~0.3M-parameter models only matched
  zero-parameter rules.
- For diagnosis, mechanistic features (from documented kernel semantics) beat
  learning on raw telemetry: 100% against 67% on held-out workloads.

So the next experiments test **prediction**, where rules have no direct
answer, rather than larger language models.

## R1. A tiny OS world model: State(t) + Action(t) → State(t+1)

> **Status:** done for CPU quota, cpuset and offered load, in one
> environment. See [`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md).
> Learned models did not beat the mechanistic model outside the training
> workloads. The next steps are in that document's *Next questions*.

**Scope:** Linux CPU scheduling under cgroup v2 limits, in disposable
containers or VMs only. The host is never the subject, and learned
recommendations are never applied.

### Data

- **Collector:** extend `tools/cpu_lab.cpp`, which already reads per-window
  telemetry through OSIE's own adapter.
- **State:** CPU utilisation, PSI cpu some/full, `cpu.stat` (`nr_periods`,
  `nr_throttled`, `throttled_usec`), runnable tasks, the cgroup quota and
  period, the CPU set, and the last k windows (k ≤ 8).
- **Action:** a change in workload intensity (number of busy threads, duty
  cycle) or a change in limits (`cpu.max` quota, `cpuset`). Changes are made
  only by the lab harness, inside the container, with the change logged.
- **Outcome:** next-window CPU saturation, throttled fraction, probe latency
  p50/p99, and change in PSI.
- **Design:**
  - Randomise the order of interventions within a run.
  - Use at least 3 runs per condition, on at least 2 hosts or VM shapes.
  - Record the kernel version, because cgroup v2 semantics depend on it.

### Splits

Splits are by **workload family**, **run** and **host**, never by window.
Adjacent windows are strongly correlated, so a random window split leaks the
answer. Hold out:
- at least one workload family (for example I/O-bound plus CPU-bound mixes);
- at least one host or VM shape not seen in training.

### Models, simplest first

Each model must beat the one before it on held-out conditions before the next
is tried:
1. A **mechanism model** from documented semantics. With a quota of q per
   period p and demand d, expected throttling ≈ max(0, d − q/p) / d,
   saturation ≈ min(1, d · period / quota).
2. Linear and ridge regression.
3. Gradient-boosted trees.
4. A tiny MLP with fewer than 50k parameters.
5. A small GRU over the last k windows with fewer than 200k parameters.
6. Graph-aware models only if the earlier ones fail specifically on topology
   effects.

### Metrics

- **Prediction:** MAE and calibration (coverage of 80% and 95% prediction
  intervals) per outcome.
- **Intervention effect:** error of the predicted *change* after an action
  against the observed change.
- Confidence intervals by bootstrap over **runs**, not windows.
- **Costs:** training compute and inference latency, alongside quality.

### Decision rule

A learned model is adopted for a question only if both hold:
- its interval-coverage and MAE improvements over the mechanism model are
  outside the run-level bootstrap CI on held-out families and hosts;
- its inference cost stays below 1 ms per prediction.

Otherwise the mechanism model stays.

**Not claimed by this design:** accurate prediction does not show causal
understanding. Bottleneck classification, performance prediction,
intervention-effect prediction and causal attribution are reported
separately.

**What OSIE would do with it:** `what_if` questions ("what happens if I halve
this container's CPU quota?") could get a *prediction labelled as such*, with
an interval. Today OSIE declines to give a number. It would still never
apply the change.

## R2. Response generation: is a compact neural generator worth it?

**Prerequisite (engineering, small):**
- Split `Engine::render` into a `ResponseGenerator` interface that consumes
  a validated `Diagnosis` and evidence. That interface cannot observe or
  authorise anything.
- Move the existing templates behind it, as the baseline.

### Candidates

1. The current templates.
2. Compositional templates that vary structure by finding type and certainty.
3. A small generator (10M–100M parameters) trained to verbalise evidence.

Candidate 3 is constrained decoding over evidence slots. All numbers and
names are copied from evidence, never generated.

### Evaluation set

Before any training, write about 300 (diagnosis, evidence) pairs drawn from
recorded sessions on macOS and Linux, with reference explanations from at
least 2 authors.

### Metrics

- **Factual consistency:** every number, name and claim must be traceable to
  evidence. This is checked automatically by matching against evidence ids.
- **Unsupported-claim rate.**
- **Usefulness:** blind pairwise preference by people who did not write the
  references.
- **Multi-turn coherence**, on scripted dialogues.
- **Costs:** CPU latency, peak RAM, training and inference cost.

### Adoption rule

A neural generator is adopted only if all three hold:
- its unsupported-claim rate is no worse than the templates', which is
  effectively 0;
- it wins usefulness preference with a 95% CI excluding 50%;
- it adds less than 50 ms and less than 200 MB.

Otherwise the templates stay. Any generator that is trained is released
separately with a model card, its evaluation data and versioned weights. It
is never described as the intent model.

## R3. Training-cost research for the intent models

Questions:
- **Equal quality at lower cost:** can the current end-to-end quality
  (65–71%) be reached with less data, fewer parameters or less compute?
- **Better data:** does a better dataset beat a bigger model? The prior
  finding was that template **diversity** helped, from 36.7% to 47.9% at
  equal volume.

### Independent evaluation data first

The biggest weakness is that rules, templates and the hand-written set share
an author. Build an evaluation set from people who did not write the rules,
with consent and personal information removed. Freeze it before any of the
experiments below.

### Experiments

Each experiment reports end-to-end accuracy against **total pipeline cost**:
data authoring, generation, training, selection and validation. Teacher-model
cost counts wherever a teacher is used.
- Active learning and hard-example mining from rule-parser disagreements.
- Curriculum learning, from short to long and from templated to free-form
  utterances.
- Knowledge-assisted features. The existing `knowledge` flag is the baseline.
- Feature selection over the hashed n-gram buckets.
- Quantisation-aware training. Post-training int8 is already lossless, so
  this targets int4 embeddings.
- Distillation, only with the teacher's cost included.

### Rule

A cost reduction is claimed only at matched quality: within the 3-seed
standard deviation on **both** held-out sets.

## R4. Temporal intelligence, next steps

Builds on the history layer added in this iteration:
- **Persistence, opt-in.** A local SQLite or flat-file store, after a privacy
  design that covers:
  - what is stored (aggregates by default; process names only with consent);
  - retention and size caps;
  - file mode 0600 under the user data directory;
  - a one-command purge.
- **More series:** cgroup `throttled_usec` deltas (so "was the CPU throttled
  before the latency spike?" can be answered), and per-interface network
  rates.
- **Statistics:** autocorrelation-corrected trend tests (for example
  Hamed–Rao) to replace the effect-size floor as the main guard. Evaluate on
  recorded traces with known injected changes, and report false-positive
  rates.

## R5. Kubernetes, read-only (after R4 settles)

Follows the design in [`KUBERNETES.md`](KUBERNETES.md):
- A typed HTTP client against the API server using the in-cluster service
  account or a kubeconfig. No `kubectl` subprocess.
- Only RBAC verbs `get`, `list` and `watch`.
- Read nodes, pods, containers, requests and limits, conditions, restart
  reasons including `OOMKilled`, and `metrics.k8s.io` where it is installed.
- Join pods to cgroups through the pod UID in the cgroup path.
- Tests run against a disposable kind or k3d cluster created and destroyed by
  CI, with no production credentials.
- **Acceptance:** cross-layer answers (pod → container → cgroup → host
  pressure) on scripted scenarios such as CPU-throttled and OOMKilled pods.
  Every claim must cite an API object or a cgroup file.

## R6. Graph semantics

- Add a `statistical_association` edge nature, distinct from `hypothesized`
  (causal). R1 and R4 need it before learned associations can enter the
  graph.
- Add thread → CPU eligibility edges, populated only from observed affinity
  (`sched_getaffinity` on Linux). Topology alone is never used to infer
  placement.
