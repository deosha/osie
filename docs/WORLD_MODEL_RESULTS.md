# CPU world model: results

Protocol: [`WORLD_MODEL_EXPERIMENTS.md`](WORLD_MODEL_EXPERIMENTS.md). Design:
[`WORLD_MODEL.md`](WORLD_MODEL.md). Raw results are in
`ml/world_model/results/*.json`, and the datasets are committed under
`ml/world_model/data/` (`*.jsonl.gz`).

**One environment only:** Linux 6.10.14-linuxkit in a Docker Desktop VM (arm64,
12 vCPUs) on an Apple M4 Pro. Nothing below is claimed to hold on bare metal,
x86-64 or other kernels. Experiment 7 (cross-hardware transfer) could not be
run.

## Summary

1. **The mechanistic model generalised best.** It has zero parameters and
   uses one fact measured from the kernel: demand ≈ usage + throttled CPU
   time. It was the best or near-best predictor of CPU usage and throttling
   on everything outside the training workloads:
   - on a held-out workload family;
   - on unseen quota values;
   - on a held-out workload class.

   On the batch class its errors were 0.005 cores (usage) and 0.009 cores
   (throttling).
2. **Tiny learned models helped only in-distribution.** A 660-parameter
   hybrid (the mechanistic prior plus an MLP residual) beat the mechanistic
   model on held-out runs of the training families, by −0.047 normalised
   error (95% CI −0.074 to −0.021). It was worse on the held-out family, by
   +0.020 (+0.003 to +0.038).

   On a held-out **workload class** (closed-loop batch threads) it failed
   badly:
   - usage error 0.40 cores, against 0.005 for the mechanistic model;
   - in a live check it predicted 1.08 cores where the kernel gave 2.04.

   It had learned a property of the training workloads, open-loop servers
   whose backlog overstates sustained demand, not a property of the OS.
3. **OSIE therefore ships the mechanistic predictor for usage and
   throttling,** with a learned residual only for CPU pressure (PSI), where
   no closed form exists. This deviates from the pre-registered rule, under
   which validation would have selected the 660-parameter hybrid. The
   decision was made after seeing held-out results. The held-out numbers for
   the shipped model are therefore **not unbiased estimates**; they need
   confirming on new workloads.
4. **Mechanism-aware features improve sample efficiency (H1).** With 1 run
   per family, an MLP on mechanism features reaches 0.330 normalised error,
   against 0.440 on generic telemetry. The gap closes with 6 runs (0.225 vs
   0.223).
5. **Latency is not predictable from what OSIE observes.** Using cgroup
   telemetry alone, p99-latency errors on the held-out family are ≈ 2.7–3.0
   log units (15–20×), and the direction of change is right only 15–17% of
   the time. OSIE therefore never predicts latency at run time.
6. **Inference is negligible.** The model has 609 effective parameters in a
   16.3 KB file. Prediction takes **1.1 µs** (p50) and 1.5 µs (p99) in C++,
   and the file loads in 0.18 ms. Python and C++ outputs agree to 1.4e-7.

## Data

| Set | Runs | Episodes | Use |
|---|---|---|---|
| train (arith, hash, short, burst) | 24 | 288 | fitting |
| val | 8 | 96 | all selection, conformal and OOD calibration |
| test (same families, unseen runs) | 8 | 96 | reporting |
| `mixed` family (held out) | 10 | 120 | reporting |
| counterfactual (all 5 families) | 10 | 120 (30 configurations × 4 quotas) | ranking |
| batch class (closed-loop threads) | 4 | 32 | reporting, collected after the main analysis |

- **Validity:** all 752 episodes collected (600 main, 120 counterfactual, 32
  batch) were valid. In every case, the applied quota or cpuset matched the
  request within 0.02.
- **Interventions in the main set:**
  - CPU quota: 329;
  - cpuset: 102;
  - offered load: 92;
  - none (control): 77.
- **Controls show no drift.** Mean change over no-intervention episodes, with
  95% run-bootstrap CI:
  - PSI −0.000 [−0.004, 0.003];
  - throttled −0.000 [−0.006, 0.006] cores;
  - usage +0.004 [−0.003, 0.012] cores.

## Measured intervention effects (randomised, run-bootstrap CIs)

| Intervention | Δ CPU usage (cores) | Δ throttled (cores) | Δ CPU PSI some |
|---|---|---|---|
| quota lowered (163 episodes, 47 runs) | −0.24 [−0.31, −0.17] | +0.69 [+0.52, +0.87] | +0.23 [+0.18, +0.29] |
| quota raised (166, 48) | +0.025 [+0.011, +0.041] | −0.22 [−0.31, −0.14] | −0.08 [−0.11, −0.05] |
| none (77, 39) | +0.004 [−0.003, +0.012] | −0.000 [−0.006, +0.006] | −0.000 [−0.004, +0.003] |

**Counterfactual runs, p99 latency of the instrumented servers.** Halving the
quota from 1 to 0.5 core raised p99 latency by ×15.4 (geometric mean, 95% CI
×8.0–×29.0, 30 configurations, minimum ×1.2).

More CPU was not always better:
- In 4 of 30 configurations, the largest candidate quota was not the best by
  more than 10%.
- For the worker-bound `mixed` family, raising the quota beyond 1 core left
  p99 unchanged; for example 2,927 → 2,945 → 2,940 ms at 1, 1.5 and 2 cores.

These effects were added to the architecture graph as `experimental` edges.
Their documentation reference names this environment. They appear in OSIE's
prediction answers labelled "experimentally supported".

## Experiments 1, 2 and 8: features × models

`mean_norm_mae` is the mean over the four targets of MAE divided by the
training standard deviation; lower is better. MLPs use 3 seeds. Δ is the
paired run-bootstrap difference against the mechanistic model; negative
means better.

| Model / features | Params | val | test | `mixed` family | Δ test vs mech [95% CI] | Δ `mixed` vs mech [95% CI] |
|---|---|---|---|---|---|---|
| no-change | 0 | 0.482 | 0.332 | 0.083 | | |
| **mechanistic** | **0** | 0.264 | 0.225 | **0.067** | | |
| ridge / generic | 104 | 0.485 | 0.495 | 0.373 | +0.270 [+0.221, +0.326] | +0.306 [+0.273, +0.338] |
| ridge / mechanism | 80 | 0.350 | 0.349 | 0.244 | +0.124 [+0.091, +0.162] | +0.177 [+0.139, +0.208] |
| gbt / generic | 43k | 0.308 | 0.340 | 0.209 | +0.116 [+0.086, +0.152] | +0.142 [+0.116, +0.170] |
| gbt / mechanism | 31k | 0.203 | 0.230 | 0.118 | +0.005 [−0.046, +0.052] | +0.051 [+0.030, +0.070] |
| gbt / both | 36k | 0.172 | 0.190 | 0.120 | −0.034 [−0.061, −0.010] | +0.053 [+0.042, +0.066] |
| mlp_h16 / generic | 484 | 0.237 | 0.223 ± 0.005 | 0.110 | +0.002 [−0.036, +0.043] | +0.030 [+0.010, +0.050] |
| mlp_h16 / mechanism | 388 | 0.208 | 0.225 ± 0.008 | 0.094 | +0.006 [−0.025, +0.037] | +0.030 [+0.011, +0.047] |
| mlp_h16 / both | 660 | 0.206 | 0.206 ± 0.011 | 0.094 | −0.006 [−0.038, +0.023] | +0.025 [+0.010, +0.043] |
| mlp_2x384 / both | 164k | **0.158** | **0.172 ± 0.005** | 0.092 | −0.060 [−0.089, −0.030] | +0.027 [+0.002, +0.048] |
| res_mlp_h16 / both (prior + residual) | 660 | 0.176 | 0.177 ± 0.002 | 0.086 | −0.047 [−0.074, −0.021] | +0.020 [+0.003, +0.038] |

Only the first seed's Δ is shown for MLPs. Ridge, GBT and the baselines are
deterministic.

**Per target, test runs (MAE):**

| Model | usage (cores) | throttled (cores) | throttled ratio | PSI some |
|---|---|---|---|---|
| no-change | 0.115 | 0.243 | 0.192 | 0.108 |
| mechanistic | 0.043 | 0.220 | 0.083 | 0.108 |
| gbt / both | 0.051 | 0.185 | 0.085 | 0.072 |
| mlp_2x384 / both | 0.056 | 0.114 | 0.104 | 0.051 |
| res_mlp_h16 / both | 0.041 | 0.183 | 0.085 | 0.063 |

**Direction of effect.** On test runs, for interventions whose true change in
throttling exceeded 0.1 cores (19 episodes), the predicted direction was
right for:
- mechanistic: 89%;
- res_mlp_h16: 100%;
- mlp_2x384: 95%;
- no-change: 0%.

## Experiment 3: data efficiency (H1)

Test `mean_norm_mae` against training runs per family (3 random subsets; mean
± std):

| Model / features | 1 run | 2 | 3 | 4 | 6 |
|---|---|---|---|---|---|
| ridge / generic | 0.543 ± 0.024 | 0.501 | 0.501 | 0.493 | 0.495 |
| ridge / mechanism | 0.412 ± 0.054 | 0.356 | 0.355 | 0.352 | 0.349 |
| gbt / generic | 0.654 ± 0.026 | 0.491 | 0.425 | 0.406 | 0.340 |
| gbt / mechanism | 0.455 ± 0.033 | 0.313 | 0.282 | 0.259 | 0.230 |
| mlp_h16 / generic | 0.440 ± 0.024 | 0.349 | 0.313 | 0.277 | 0.223 |
| mlp_h16 / mechanism | 0.330 ± 0.020 | 0.286 | 0.256 | 0.248 | 0.225 |
| res_mlp_h16 / both | **0.256 ± 0.029** | 0.211 | 0.204 | 0.198 | 0.177 |
| *mechanistic (no data)* | *0.225* | | | | |

- **H1 is supported for small data.** Mechanism features need about half the
  runs to reach a given error with GBT and MLPs:
  - gbt/mechanism reaches 0.313 with 2 runs; gbt/generic needs 6 runs for
    0.340;
  - mlp/mechanism reaches 0.286 with 2 runs; mlp/generic reaches 0.277 with 4.
  - The advantage disappears for the MLP at 6 runs per family.
- **But the mechanistic model with no data at all (0.225)** is matched only
  by learned models trained on 6 runs per family (≈ 30 minutes of
  collection), except the residual hybrid, which needs 2.

## Experiment 4: history

| Model | 1 window | 2 | 4 | 6 |
|---|---|---|---|---|
| gbt / both | 0.210 | 0.192 | 0.194 | 0.190 |
| mlp_h16 / both | 0.207 | 0.203 | 0.196 | 0.206 |
| res_mlp_h16 / both | 0.199 | 0.179 | 0.185 | 0.177 |
| GRU over raw windows (2.4k / 6.9k params) | | | | 0.219 / 0.202 |

- History beyond one 0.5 s window gives a small improvement (about 0.01–0.02)
  that is within seed spread for the MLP.
- A recurrent model over raw windows is not better than static summary
  features.
- Shorter-than-0.5 s windows were not tested.

## Experiment 5: unseen quota values

Quotas 1.5 and 2.5 cores were removed from training as post-intervention
values. Test on 18 such episodes:

| | no-change | mechanistic | gbt/both | mlp_h16/generic | mlp_h16/mechanism | res_mlp_h16/both |
|---|---|---|---|---|---|---|
| mean_norm_mae | 0.134 | **0.064** | 0.085 | 0.136 | 0.097 | 0.072 |

The mechanistic model, which has no notion of seen and unseen values,
generalises best. Mechanism features roughly halve the error of generic ones
for MLPs and GBT, which is evidence for H3. With only 18 episodes the CIs
are wide.

## H3: physically implausible raw outputs

These are raw model outputs before OSIE's clamping. A violation is either of:
- usage outside [0, capacity after the change];
- any negative quantity, or a fraction above 1.

| Model | test: any / > 0.05 | `mixed`: any / > 0.05 |
|---|---|---|
| mechanistic | 0.00 / 0.00 | 0.00 / 0.00 |
| no-change | 0.15 / 0.07 | 0.02 / 0.02 |
| ridge / generic | 0.42 / 0.38 | 0.57 / 0.50 |
| gbt / generic | 0.51 / 0.42 | 0.77 / 0.63 |
| gbt / both | 0.68 / 0.25 | 0.88 / 0.23 |
| mlp_h16 / generic | 0.52 / 0.20 | 0.67 / 0.23 |
| mlp_h16 / mechanism | 0.59 / 0.12 | 0.83 / 0.21 |
| mlp_2x384 / both | 0.56 / 0.03 | 0.84 / 0.03 |
| res_mlp_h16 / both | 0.64 / 0.04 | 0.82 / 0.03 |

- Small violations (for example −0.01 cores of throttling) are common for
  every learned model, and harmless after clamping.
- **Material violations fall from 20–63% to 3–4%** when the mechanism is
  built into the model (prior plus residual). Mechanism *features* alone help
  less.
- OSIE always clamps outputs to physical bounds and flags clamping.

## Uncertainty, OOD and abstention

These figures are for the validation-selected `res_mlp_h16`.

**Split-conformal coverage** (calibrated on validation runs):

| | test 80% / 95% | `mixed` 80% / 95% |
|---|---|---|
| usage | 0.77 / 0.99 | 0.93 / 0.98 |
| throttled cores | 0.82 / 0.95 | 0.92 / 0.99 |
| throttled ratio | 0.82 / 0.95 | 0.93 / 0.97 |
| PSI some | 0.89 / 0.99 | 0.93 / 0.99 |

**OOD detector.** It flags episodes outside the training feature ranges, or
above the validation 99th percentile of the mean squared z-score.
- It flagged **2.1% of test episodes**, whose error was 6× higher (0.99
  normalised, against 0.16 for accepted episodes).
- It flagged **5% of the held-out family**, on which errors were low anyway.
- It flagged **19 of 32 batch episodes** (59%). That is useful but
  incomplete: it would have let 13 badly predicted batch episodes through.

**False-safe check.** Among quota changes predicted to leave throttling ≤ 0.1
cores, how many actually had > 0.5 cores of throttling:
- learned hybrid: 0 of 34 (test) and 0 of 58 (`mixed`);
- mechanistic: 4 of 44 on test (9%) and 0 of 68 on `mixed`.

**The shipped model inherits this 9% in-distribution false-safe rate from the
mechanistic predictor.** That is the price of its better behaviour on unseen
workload classes.

## Shipped model (`models/world_cpu.osiewm`)

`cpu-world-mech_psi_mlp_h16-both-v1`:
- mechanistic usage, throttled cores and throttled ratio;
- an MLP residual (16 hidden units, 609 effective parameters) for PSI only;
- split-conformal intervals and OOD abstention.

The figures below are from the C++ artifact, scored with its clamping and
abstention. Each cell is MAE on accepted predictions, shown as
shipped / mechanistic.

| Set | Abstained | usage | throttled cores | throttled ratio | PSI some |
|---|---|---|---|---|---|
| test | 2 / 96 | 0.038 / 0.038 | 0.192 / 0.192 | 0.073 / 0.073 | **0.047** / 0.101 |
| `mixed` family | 6 / 120 | 0.049 / 0.049 | 0.025 / 0.025 | 0.025 / 0.025 | 0.042 / **0.031** |
| counterfactual | 5 / 120 | 0.130 / 0.130 | 0.448 / 0.448 | 0.176 / 0.176 | **0.072** / 0.204 |
| batch class | 19 / 32 | 0.006 / 0.006 | 0.011 / 0.011 | 0.036 / 0.036 | **0.087** / 0.199 |

For PSI, the "mechanistic" column means PSI unchanged.

**What was given up.** On the counterfactual runs (training families, high
utilisation), the validation-selected learned hybrid was better on usage
(0.060 vs 0.130 cores) and throttling (0.306 vs 0.448 cores).

**Interval coverage of the shipped model** (80% / 95%):

| Set | usage | throttling | PSI |
|---|---|---|---|
| test | 0.78 / 1.00 | 0.81 / 0.94 | 0.86 / 0.99 |
| counterfactual | 0.71 / 0.89 | 0.70 / 0.86 | — |
| batch class | — | — | 0.69 / 1.00 |

The counterfactual runs under-cover because they concentrate on
high-utilisation configurations.

### Live check on a real kernel

A disposable container (`--cpus 1`, 4-CPU cpuset) ran 3 spinning threads.
OSIE was asked about a 2-core quota; the quota was then applied with
`docker update` and measured over 3 s:

| | predicted (80% interval) | measured |
|---|---|---|
| CPU usage | 2.00 cores (1.96–2.00) | 2.04 |
| throttled CPU time | 1.00 cores (0.84–1.16) | 1.04 |
| throttled periods | 100% | 100% |
| CPU PSI some | 6.6% (0–18.6%; 95%: 0–34.9%) | **33.2%** |

Usage and throttling were right. **PSI was wrong**: the measured value was
outside the 80% interval and just inside the 95% one. The learned PSI residual
carries the same workload-class bias as the rejected hybrid. Treat PSI
predictions as weak.

## Latency (lab only, never at run time)

p99 latency, MAE in log units. ≈ 0.4 means a typical 1.5× error; 2.7 means
15×.

| Features | Model | val | test | `mixed` | direction acc. (test / `mixed`) |
|---|---|---|---|---|---|
| — | no-change | 1.10 | 0.85 | 0.63 | — |
| OSIE-observable only | gbt | 0.59 | 0.73 | **2.78** | 0.83 / 0.15 |
| OSIE-observable only | mlp_h16 | 0.86 | 0.91 | **3.02** | 0.78 / 0.17 |
| + application metrics | gbt | 0.46 | 0.53 | 1.00 | 0.87 / 0.31 |
| + application metrics | mlp_h16 | 0.54 | **0.47** | **0.59** | 0.93 / 0.62 |

Even with application metrics, held-out-family latency errors stay around
1.8×. With what OSIE can observe, predicting latency direction on an unseen
family is worse than chance.

**Ranking candidate quotas** (counterfactual, 30 configurations × 4 quotas;
outcome = observed p99):

| Ranker | Spearman | top-1 | mean regret | "will raising the quota improve p99?" |
|---|---|---|---|---|
| "more CPU is better" | 0.81 | 0.53 | 8.3 ms | 0.60 |
| mechanistic (rank by predicted throttling) | **0.88** | **0.60** | 9.1 ms | **0.80** |
| lab latency model (gbt, + application metrics) | 0.81 | 0.53 | **1.6 ms** | 0.77 |

## Cost (total pipeline, not only training)

| Item | Cost |
|---|---|
| Lab image build | ≈ 1 min |
| Collection, main set | 47 min wall (2 lanes × 50 runs × 110 s). 4,605 CPU-core-seconds in experiment containers, plus 25 neighbour runs at ≤ 1 core × 110 s. |
| Collection, counterfactual / batch | ≈ 10 min / 3 min. 897 / 423 core-seconds. |
| Observer overhead | 97 µs per window p50 (280 µs p99), about 0.02% of a 500 ms window |
| Model selection and experiments | 219 s wall, about 960 CPU-seconds per full run of `experiments.py`. It was run 3 times (one partial-data smoke run and two full runs after bug fixes), about 10 CPU-minutes in total. |
| Training the shipped model | well under 1 s (MLP with 16 hidden units, early stopping) |
| Inference (C++) | 1.1 µs p50, 1.5 µs p99, load 0.18 ms; 16.3 KB file |
| Inference (Python, for comparison) | 20–40 µs per call (MLP/ridge); 26 ms for GBT |
| Energy | not measured (needs privileged counters on macOS) |

The dominant cost is **data collection**. On this evidence, getting
mechanistic-model quality from learning alone needs ≈ 6 runs per family
(≈ 30 minutes of collection per 4 families). The mechanism costs nothing to
collect, but it took the throttling measurement in `WORLD_MODEL.md` to
discover. The claim that structure reduces the cost of training is supported
here in that narrow, measured sense: zero training data reached in-family
errors that learned models needed ~30 minutes of collection to match, and
better errors outside the training workloads. It is not shown as a general
law.

## Hypotheses: verdict

| Hypothesis | Verdict | Evidence |
|---|---|---|
| H1: structure improves sample efficiency | **Supported for small data** | Learning curves: about half the runs for GBT and MLP. The gap closes for the MLP at 6 runs/family. The zero-data mechanistic model is the strongest form of the effect. |
| H2: < 1M-parameter models learn useful dynamics | **Supported in-distribution only** | 660 and 164k-parameter models beat both baselines on held-out runs of training families (CIs exclude 0). They do not beat the mechanistic model on a held-out family, and the 660-parameter hybrid failed on a held-out workload class. |
| H3: architecture constraints improve reliability | **Supported** | Building the mechanism in cuts material implausible outputs from 20–63% to 3–4%. Mechanism features halve unseen-quota error. The pure mechanism generalised best across workload classes. |

## Failed or weak results

- The validation-selected model was unsafe outside its workload class. That
  was not detected until a held-out *class* was tested. A held-out *family*
  of the same class (open-loop servers) did not reveal it.
- The OOD detector missed 41% of the batch-class episodes.
- The PSI prediction is weak (live check: 6.6% predicted, 33.2% measured).
- The shipped model has a 9% in-distribution false-safe rate for throttling.
- Latency prediction from OS telemetry fails on unseen families.
- Cross-hardware, cross-kernel and bare-metal transfer are untested.

## Threats to validity

- **Single environment.** One VM, one kernel, arm64, virtualised CPUs. The
  measured `throttled_usec` semantics may differ on other kernel versions.
- **Synthetic workloads.** Five open-loop request families and one closed-loop
  batch class; no real applications.
- **Host interference.** The Docker VM also ran the developer's other
  long-lived containers, and the macOS host was in normal use. In particular,
  a 90 s training smoke test ran on the host during the first ~15 minutes of
  collection. Host load is recorded (`g_host_busy`, run-level load average),
  but its effect on these episodes was not separated out.
- **Post-hoc deployment choice.** The shipped composite was chosen after
  seeing held-out results (see Summary item 3).
- **Small held-out sets.** 8 test runs, 18 unseen-quota episodes and 4 batch
  runs give wide CIs.
- **Carry-over.** Each episode restarts the workload with an empty queue.
  Some carry-over through CPU caches or the scheduler is possible. The
  no-intervention controls show no measurable drift.

## Next questions

1. Collect closed-loop and mixed-class workloads, and real services (for
   example nginx or postgres under a load generator), in the training data.
   Then re-test whether a learned residual survives a held-out class.
2. An OOD signal that targets the failure seen here. One candidate: whether
   the cgroup's runnable demand changes when capacity changes (closed loop)
   or not (open loop). It is observable over time without application
   metrics.
3. A PSI model with a mechanistic prior (queueing-theoretic), instead of the
   "unchanged" baseline.
4. Repeat on bare-metal x86-64 and on another kernel version (Experiment 7).
5. Measure energy where privileged counters are available.


---

## v0.2 results

The protocol is in [`WORLD_MODEL_EXPERIMENTS.md`](WORLD_MODEL_EXPERIMENTS.md)
("v0.2 protocol"). In brief:
- all selection used only development data, by leave-one-family-out (LOFO)
  cross-validation;
- intervals were calibrated on separate runs;
- the model was frozen and committed before any confirmatory data existed.

Every number below comes from a file in `ml/world_model/results/v02/`.

### 1. Demand estimation (`demand_study*.json`)

The v0.1 mechanism estimates CPU demand as usage + `throttled_usec` per
second. The lab knows two independent ground truths:

| Ground truth | Definition |
|---|---|
| **Offered** demand | arrivals/s × measured CPU per request, capped by parallelism |
| **Runnable** demand | while a request backlog exists, every worker is runnable |

Under a binding quota:

| Data | Windows | Estimate vs runnable: MAE / bias (cores) | Estimate vs offered: bias | usage + PSI vs offered: bias |
|---|---|---|---|---|
| development (6.10, aarch64) | 263 | 0.116 / −0.017 | +1.096 | −0.013 |
| confirmatory, local (6.10, aarch64) | 146 | 0.135 / −0.089 | +0.697 | −0.357 |
| confirmatory, runner (6.17, x86-64) | 152 | 0.145 / −0.012 | +0.815 | −0.482 |
| confirmatory, runner (6.17, arm64) | 146 | 0.133 / −0.084 | +0.691 | −0.519 |

For always-runnable batch threads the estimate is within 0.008 cores. When
the quota does not bind, usage alone is within 0.012–0.024 cores.

**Finding.** `usage + throttled_usec/s` measures *runnable* demand, and that
semantics (throttled time summed over CPUs) held on Linux 6.10 and 6.17, on
arm64 and on x86-64. It is not the offered load of a request server.

For prediction over OSIE's 3 s horizon, the runnable estimate was still the
better choice on development data: LOFO error 0.169 against 0.271 for
usage + PSI.

### 2. Why learned corrections fail on batch threads (`failure_analysis.json`)

The table gives the mean usage error of the mechanism (observed − predicted,
in cores) after a quota change from a throttled state, on development data.

| Family | Quota raised | Quota lowered |
|---|---|---|
| arith | −0.357 (16) | +0.000 (8) |
| burst | −0.646 (30) | +0.003 (15) |
| short | −0.380 (20) | +0.003 (11) |
| mixed | −0.288 (7) | — |
| batch threads | −0.004 (14) | +0.004 (11) |

When a quota is **lowered**, the mechanism is exact in every family. When it
is **raised**, open-loop servers use less than their runnable demand once the
backlog drains: offered load is below runnable demand.

A learned correction fitted to request servers learns "shrink after
increases". That is wrong for batch threads, which really do use their
runnable demand. This is the v0.1 failure, explained, and it is why no
learned correction is applied to usage or throttling in v0.2.

### 3. Selection (development data only; `dev_selection.json`)

LOFO over six workload classes on the 43 fit runs. "Core" error = mean of the
normalised MAE of usage and throttled time; lower is better.

| Candidate | LOFO core | Worst held-out family | Throttled ratio | CPU pressure |
|---|---|---|---|---|
| **A: mechanism (0 parameters)** | **0.169** | **0.332** | **0.205** | 0.593 (no change) |
| B: best direct MLP (h16, 660 params, 5 seeds) | 0.271 ± 0.005 | 0.68 | 0.360 | 0.284 |
| C: best residual MLP (h16, 5 seeds) | 0.236 ± 0.006 | 0.41 | 0.252 | 0.301 |
| Best gradient-boosted residual | 0.221 | 0.49 | – | – |
| D: rule-gated residual | 0.177 | 0.33 | 0.212 | 0.561 |
| E: learned logistic gate | 0.213 | 0.32 | 0.233 | 0.398 |
| F: abstain on learned/mechanism disagreement (accepted only) | 0.115 | 0.18 | 0.174 | 0.222 |
| mechanism on F's accepted episodes | 0.089 | 0.18 | 0.159 | 0.356 |
| usage + PSI demand estimate | 0.271 | 0.58 | 0.240 | 0.593 |
| no change | 0.514 | 1.38 | 0.501 | 0.593 |
| CPU pressure only: separate h8 residual, quantile loss | – | – | – | **0.299** |

Notes:
- Target-specific losses (MAE, Huber, quantile; under-prediction × 3 for
  throttling) did not beat the shared residual on usage or throttling. The
  asymmetric loss made throttling error worse (0.364 against 0.292).
- Policy F's apparent gain comes from *which episodes it keeps*: the
  mechanism alone on the same episodes is better still.
- A larger network never helped: 8–64 hidden units span 0.233–0.26 for the
  residual form.

**Selected:** the mechanism for usage, throttled time and throttled ratio. For
CPU pressure, which has no closed form, a 305-parameter residual, ungated (gating
removed its benefit) and marked *experimental*.

### 4. Confirmatory results

Data collected after the model was frozen (commit 9002fe6):

| Environment | Runs | Episodes |
|---|---|---|
| local Docker Desktop VM (6.10, aarch64) | 40 | 392 |
| GitHub-hosted x86-64 runner (6.17) | 30 | 294 |
| GitHub-hosted arm64 runner (6.17) | 30 | 294 |

Workload groups:
- `held_in`: the development families with new seeds;
- `new`: four families that did not exist when the model was frozen;
- `batch`: closed-loop batch threads.

The error and coverage numbers are *forced*, i.e. without the environment
check, to measure transfer. The "deployed" table shows what a user actually
gets.

#### MAE against the no-change baseline (cores; ratio and pressure as fractions)

| Environment | Group | Episodes / runs | usage | throttled time | throttled ratio | CPU pressure |
|---|---|---|---|---|---|---|
| local 6.10 aarch64 | held_in | 200 / 20 | 0.079 [0.049, 0.110] vs 0.155 | 0.164 [0.106, 0.233] vs 0.294 | 0.087 [0.050, 0.130] vs 0.201 | 0.065 [0.050, 0.081] vs 0.125 |
| local 6.10 aarch64 | new | 160 / 16 | 0.052 [0.035, 0.073] vs 0.205 | 0.231 [0.149, 0.317] vs 0.401 | 0.061 [0.033, 0.097] vs 0.217 | 0.077 [0.055, 0.102] vs 0.133 |
| local 6.10 aarch64 | batch | 32 / 4 | 0.004 [0.003, 0.005] vs 0.828 | 0.011 [0.006, 0.016] vs 0.833 | 0.065 [0.022, 0.098] vs 0.394 | 0.112 [0.084, 0.145] vs 0.340 |
| runner 6.17 x86-64 | held_in | 150 / 15 | 0.095 [0.049, 0.156] vs 0.201 | 0.249 [0.182, 0.320] vs 0.469 | 0.065 [0.035, 0.100] vs 0.260 | 0.068 [0.051, 0.085] vs 0.170 |
| runner 6.17 x86-64 | new | 120 / 12 | 0.104 [0.049, 0.172] vs 0.268 | 0.268 [0.132, 0.431] vs 0.588 | 0.096 [0.046, 0.151] vs 0.251 | 0.095 [0.067, 0.121] vs 0.158 |
| runner 6.17 x86-64 | batch | 24 / 3 | 0.004 [0.002, 0.007] vs 0.708 | 0.009 [0.007, 0.011] vs 0.716 | 0.090 [0.000, 0.192] vs 0.214 | 0.121 [0.088, 0.175] vs 0.238 |
| runner 6.17 arm64 | held_in | 150 / 15 | 0.086 [0.043, 0.138] vs 0.176 | 0.213 [0.150, 0.277] vs 0.391 | 0.062 [0.029, 0.097] vs 0.236 | 0.066 [0.047, 0.085] vs 0.157 |
| runner 6.17 arm64 | new | 120 / 12 | 0.086 [0.036, 0.158] vs 0.268 | 0.268 [0.133, 0.433] vs 0.562 | 0.113 [0.060, 0.167] vs 0.232 | 0.092 [0.066, 0.116] vs 0.152 |
| runner 6.17 arm64 | batch | 24 / 3 | 0.003 [0.002, 0.004] vs 0.709 | 0.007 [0.002, 0.012] vs 0.716 | 0.089 [0.000, 0.167] vs 0.219 | 0.118 [0.083, 0.174] vs 0.244 |

#### Interval coverage (95% / 80%), model A → model B

| Environment | Group | usage | throttled time | throttled ratio | CPU pressure (same in A and B) |
|---|---|---|---|---|---|
| local 6.10 aarch64 | held_in | 0.98/0.81 → 1.00/0.83 | 0.97/0.84 → 0.96/0.88 | 0.95/0.81 → 0.97/0.91 | 0.95/0.84 |
| local 6.10 aarch64 | new | 0.99/0.84 → 1.00/0.84 | 0.96/0.80 → 0.95/0.91 | 0.97/0.84 → 0.97/0.93 | 0.94/0.78 |
| local 6.10 aarch64 | batch | 1.00/1.00 → 1.00/1.00 | 1.00/1.00 → 1.00/1.00 | 0.97/0.91 → 1.00/0.94 | 0.97/0.66 |
| runner 6.17 x86-64 | held_in | 0.95/0.85 → 0.99/0.85 | 0.97/0.77 → 0.99/0.91 | 0.95/0.89 → 1.00/0.94 | 0.95/0.83 |
| runner 6.17 x86-64 | new | 0.95/0.85 → 0.96/0.84 | 0.95/0.81 → 1.00/0.91 | 0.92/0.82 → 1.00/0.91 | 0.95/0.74 |
| runner 6.17 x86-64 | batch | 1.00/1.00 → 1.00/1.00 | 1.00/1.00 → 1.00/1.00 | 0.92/0.88 → 1.00/0.88 | 0.96/0.58 |
| runner 6.17 arm64 | held_in | 0.97/0.85 → 0.99/0.85 | 0.97/0.81 → 0.99/0.94 | 0.96/0.90 → 0.99/0.95 | 0.95/0.83 |
| runner 6.17 arm64 | new | 0.97/0.87 → 0.97/0.85 | 0.95/0.81 → 1.00/0.92 | 0.91/0.81 → 1.00/0.90 | 0.92/0.75 |
| runner 6.17 arm64 | batch | 1.00/1.00 → 1.00/1.00 | 1.00/1.00 → 1.00/1.00 | 0.96/0.88 → 1.00/0.88 | 0.96/0.46 |

#### Throttled-time 95% coverage by regime (all groups), A → B

| Environment | pre unsaturated, throttled after | pre saturated, throttled after | not throttled after |
|---|---|---|---|
| local 6.10 aarch64 | 0.72 → 0.87 (n=47) | 1.00 → 1.00 (n=61) | 1.00 → 0.97 |
| runner 6.17 x86-64 | 0.74 → 0.97 (n=31) | 0.97 → 1.00 (n=69) | 1.00 → 1.00 |
| runner 6.17 arm64 | 0.76 → 0.97 (n=29) | 0.97 → 1.00 (n=66) | 0.99 → 0.99 |

#### Deployed behaviour (what a user gets)

| Environment | Group | Withheld | Main reasons |
|---|---|---|---|
| local 6.10 aarch64 | held_in | 2% | feature g_host_busy (3), feature h_usage_std (2), the observed state is unlike the training data (1) |
| local 6.10 aarch64 | new | 32% | feature g_pids (39), feature g_host_busy (16), the observed state is unlike the training data (8) |
| local 6.10 aarch64 | batch | 0% |  |
| runner 6.17 x86-64 | held_in | 100% | feature g_host_cpus (150), CPU architecture x86_64 was not validated (150), kernel 6.17 is outside the validated range 6.10-6.10 (150) |
| runner 6.17 x86-64 | new | 100% | feature g_host_cpus (120), CPU architecture x86_64 was not validated (120), kernel 6.17 is outside the validated range 6.10-6.10 (120) |
| runner 6.17 x86-64 | batch | 100% | feature g_host_cpus (24), CPU architecture x86_64 was not validated (24), kernel 6.17 is outside the validated range 6.10-6.10 (24) |
| runner 6.17 arm64 | held_in | 100% | feature g_host_cpus (150), kernel 6.17 is outside the validated range 6.10-6.10 (150), feature g_host_busy (19) |
| runner 6.17 arm64 | new | 100% | feature g_host_cpus (120), kernel 6.17 is outside the validated range 6.10-6.10 (120), the observed state is unlike the training data (45) |
| runner 6.17 arm64 | batch | 100% | feature g_host_cpus (24), kernel 6.17 is outside the validated range 6.10-6.10 (24), the observed state is unlike the training data (13) |

#### Clamping (share of predictions whose raw output was outside physical bounds)

- local 6.10 aarch64: usage 0.0%, throttled time 0.0%, throttled ratio 0.0%, CPU pressure 12.2%
- runner 6.17 x86-64: usage 0.0%, throttled time 0.0%, throttled ratio 0.0%, CPU pressure 13.6%
- runner 6.17 arm64: usage 0.0%, throttled time 0.0%, throttled ratio 0.0%, CPU pressure 14.3%


**Findings.**

- **Transfer.** The frozen predictor beat no-change on every target, in every
  workload group and in all three environments: two kernels it never saw,
  x86-64 it never saw, and four new workload families. It reduced usage error
  by 49–75% for request servers and 99% for batch threads.
- **Coverage of model A, the first frozen model.** Coverage was close to
  nominal overall (95%: 0.91–1.00), but too low where it matters: when an
  unsaturated cgroup becomes throttled after a cut, 95% coverage was
  0.72–0.76. Its throttling assessment never said "low": 0 of 288 changes locally,
  and 0 of 418 on the runners in the secondary analysis below. Its intervals
  were wide everywhere.
- **Model B.** B uses regime-conditional intervals, developed on development
  data after the local results. It was frozen with a pre-registered decision
  rule before the runner data were read
  (`results/v02/decision_rule_A_vs_B.md`). On the runners it raised that
  regime's 95% coverage to 0.97 in both environments, with overall coverage
  0.96–1.00. B met the rule and replaced A as the shipped model.
- **Caveat on the decision rule.** Its third criterion (false-safe among "low"
  assessments) was satisfied only vacuously on the runner data. Every forced
  prediction there was out of distribution (below), so nothing was assessed.
- **Secondary analysis, not pre-registered** (`secondary_assessment.json`).
  Ignoring the out-of-distribution check, B would have assessed 93 (x86-64)
  and 105 (arm64) quota or cpuset changes as "low" throttling risk. None of
  the 198 was in fact throttled above 0.25 cores, and B never called any of
  the 142 actually-throttled changes "low". Zero in 198 does not mean zero
  risk: the rule-of-three 95% upper bound is about 1.5%. On the local data,
  which motivated B, the count was 142 with 0 false-safe. A said "low" 0 times.
- **The deployed system did what it promises, and that exposed a defect.**
  - Inside the envelope (local) it withheld 2% of predictions for held-in
    workloads, 32% for new families and 0% for batch threads. The new-family
    withholds came mostly from `wide` (65%; its task count `g_pids` is outside
    the training range) and `closed` (42%).
  - On the runners it withheld 100%, as designed: the architecture and kernel
    are outside the envelope. But the feature-range check also fired on
    **every** runner episode because of the host CPU count (`g_host_cpus`:
    4 there, 12 in all training data).
  - That feature is constant in the training data, so the current model
    effectively refuses on any host without exactly 12 CPUs, even inside the
    envelope. The mechanistic targets do not depend on it. This is a real
    portability defect (audit item W8).
- **CPU pressure stays experimental.**
  - It beat no-change everywhere: MAE 0.065–0.121 against 0.125–0.340.
  - 95% coverage was 0.92–0.97, but 80% coverage was only 0.46–0.66 for batch
    threads and 0.74–0.78 for new families.
  - 12–14% of its raw outputs were clamped at 0: the residual over-corrects
    when the current pressure is near zero.
- **No other clamping.** Usage, throttled time and ratio were never clamped:
  they are bounded by construction.

### 5. Hypotheses, revisited

The long-run hypothesis was that explicit OS mechanisms plus selective
lightweight learning give more reliable prediction under shift than learned
world models.

- **Mechanisms beat learning under shift: supported.** Zero parameters beat
  every learned and hybrid model under family shift in development. On
  confirmatory data from new families, two new kernels and a new
  architecture, the mechanism's error stayed within 0.003–0.104 cores for
  usage.
- **Selective learning adds value: supported only where no mechanism exists**
  (CPU pressure). Every form of selective residual tested (rule gate, learned
  gate, disagreement-based abstention) was no better than the mechanism alone
  for usage and throttling. The failure analysis shows why: the residual
  encodes a property of the training workloads (open-loop backlog), not of
  the kernel.
- **What learning *calibration* adds: supported.** Regime-conditional
  intervals turned an uninformative safety assessment into one that, in the
  secondary analysis, separated 198 safe changes from 142 throttled ones
  without a single false "low".

### 6. Not done

- **Hardware and hosts.**
  - No bare-metal host and no kernel older than 6.10 was tested.
  - Both runner VMs had 4 vCPUs. With `g_host_cpus` constant in training, the
    model cannot be evaluated *as deployed* on them, only forced.
- **Workloads.** Real services with application-level backpressure, garbage
  collection or caches were not tested. Neither was I/O-bound blocking beyond
  sleep-based blocking.
- **Envelope.** It was not widened, although the runner data support 6.17 on
  x86-64 and arm64. Widening it on the strength of the confirmatory data
  would make them training evidence. A future model should declare the wider
  envelope and be confirmed on new runs. The same model should drop or
  re-range host-CPU-count features (W8).
- **Calibration sample.** The rho ≥ 1.3 regime has only 17 calibration runs,
  below the 19 needed for a finite-sample 95% guarantee. Its three 95%
  half-widths use a flagged conservative fallback (the largest residual).
