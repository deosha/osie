# CPU world model

OSIE can answer one kind of hypothetical question on Linux: *what is likely to
happen to a cgroup's CPU usage, throttling and CPU pressure if its CPU quota,
cpuset size or offered load changes?* It answers from documented cgroup
semantics, a small learned correction for CPU pressure, and recent
observations. **It never applies the change.**

**Status (v0.2): experimental, off by default.** Enable it with
`--experimental` or `OSIE_EXPERIMENTAL_PREDICT=1`. Even then it predicts only
inside the model's support envelope (below); everywhere else it says
"Prediction unavailable" and why. Where it was validated, and how well it did
on data collected after the model was frozen, is in
[`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md).

### The v0.2 model

| Target | Predictor | Learned parameters | Status |
|---|---|---|---|
| CPU usage | mechanism: min(demand, capacity after the change) | 0 | validated within the envelope |
| Throttled CPU time | mechanism: demand − capacity when the quota binds | 0 | validated within the envelope |
| Throttled-period ratio | mechanism: 1 when the quota binds, else 0 | 0 | validated within the envelope |
| CPU pressure (PSI some) | no-change + learned residual (8 hidden units, quantile loss) | 305 | **experimental**: interval coverage on new workloads not established |

Intervals for the three mechanistic targets are **regime-conditional**:
separate half-widths when demand/capacity after the change is below 0.7,
between 0.7 and 1.3, or at least 1.3. Below 0.7 the throttling interval
is exactly 0, which lets the assessment say "low". The first frozen model
used one half-width per target. It under-covered the cases where throttling
appears after a cut, and it never said "low". It was replaced by a
pre-registered rule (see the results).

The selection between these and every learned alternative is described in
[`WORLD_MODEL_EXPERIMENTS.md`](WORLD_MODEL_EXPERIMENTS.md) ("v0.2 protocol").
In short: no learned model, residual or gate beat the mechanism for usage
and throttling on held-out workload families; for CPU pressure, where the
mechanism has no closed form, the learned residual halved the error.

The experiments behind this model, and its limits, are in
[`WORLD_MODEL_EXPERIMENTS.md`](WORLD_MODEL_EXPERIMENTS.md) (protocol) and
[`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md) (results).

```text
$ osie predict --cpu-quota 2 --name api
OSIE prediction (hypothetical: nothing will be changed)
Target: api (pid 4242), cgroup /system.slice/api.service
CPU quota: 1.00 cores -> 2.00 cores
...
No changes were applied.
```

In an interactive session: `predict cpu quota 1.5 for nginx` or
`predict cpuset 2 for pid 123`.

## Formulation

OSIE treats a cgroup as a partially observed dynamic system:

- S(t): the cgroup's state over the last *k* observation windows (default 6 ×
  0.5 s). Inputs are CPU usage, throttled periods and time, CPU PSI, quota,
  cpuset size, number of tasks, and host utilisation.
- A: a hypothetical intervention. It can be a CPU quota (cores), a cpuset size
  (CPUs) or an offered-load multiplier. The intervention is encoded separately
  from the state.
- Y: the outcome over the next 3 s, after a 1 s settling period. That is CPU
  usage, throttled CPU time, the fraction of throttled periods and CPU
  pressure.

The model estimates E[Y | S, do(A=a)]. **The training data come from
randomised interventions.** Each episode's intervention was drawn at random,
independently of the state, and applied by the experiment harness. Under that
design, the regression targets intervention effects for the conditions that
were sampled. That holds within the experimental environment only. Outside it,
the model's predictions are associations, and OSIE says so.

## Components

| Part | Where | Role |
|---|---|---|
| Window construction | `predict::make_window` (`src/prediction/features.cpp`) | Turns two cgroup snapshots into rates. Missing kernel statistics stay missing. |
| Feature encoder | `predict::encode` | One versioned schema (`kFeatureSchemaVersion`) is used by the lab and by OSIE, so training and runtime features cannot drift. |
| Mechanism-aware features | `m_*` features | Capacity = min(quota, cpuset). Demand ≈ usage + throttled CPU time. Projected pressure, headroom and excess. The target is never encoded. |
| Model | `predict::WorldModel` (`src/prediction/world_model.cpp`), file `models/world_cpu.osiewm` | Linear or one-hidden-layer MLP in native C++, with no runtime dependencies. |
| Physical bounds | `WorldModel::predict` | Usage is limited to [0, capacity after the change], fractions to [0, 1]. Clamped outputs are flagged. |
| Uncertainty | group-aware split-conformal half-widths | 80% and 95% intervals, calibrated on calibration runs that no other step used. One episode per run is sampled (episodes of a run are correlated), the finite-sample quantile is taken over runs, and the median over 500 draws is used. Intervals for usage allow 3% above capacity: measured usage exceeds the quota by up to 2.2% (accounting granularity). |
| Abstention | `WorldModel::predict`, `predict_in` | Withholds the prediction on any of: a machine outside the support envelope (architecture, kernel range, cgroup version), a capacity outside the validated range, cgroup throttling counters inconsistent with the validated accounting semantics, an action outside the validated range, a feature outside the training range, an unusual state (mean squared z-score above the calibration 99th percentile), missing telemetry, too little history, or an interval too wide to be useful. |
| Throttling assessment | `WorldPrediction::throttling_assessment` | For quota and cpuset changes: `low` only if the *upper* bound of the 95% interval of throttled time is below `safe_throttle_max` (0.25 cores); `likely` if the lower bound is above it; `possible` otherwise; `unknown` when withheld. A point estimate alone never makes a change look safe. |
| Engine integration | `Engine::predict` | Resolves the process to its cgroup, observes the model's history window through the audited `ObservationBroker`, evaluates the model and renders the answer. Mechanisms come from the architecture graph. |
| Capability | `observe.predict.cpu_world_model` | Kind `observe`, no side effects, Linux only. Policy-checked and audited like every other observation. |

### Why demand ≈ usage + throttled time

On Linux 6.10, cgroup v2 `cpu.stat throttled_usec` **sums throttled time
across CPUs**. It is not wall time. Measured in the lab container, 3 busy
threads under a 1-core quota for ≈ 2.07 s gave:
- `usage_usec` +2.07 s, i.e. 1.0 core;
- `throttled_usec` +4.21 s, i.e. 2.0 cores.

The sum is the 3 runnable threads. Under a 2-core quota the counters gave
2.1 + 1.05 ≈ 3. OSIE therefore estimates demand as usage + throttled time per
second, bounded by the cpuset, and **does not clamp the throttled share at
1**. This semantics is kernel-version-dependent; older kernels may report it
differently, and the model abstains when the feature lies outside its
training range.

v0.2 validated this against an independent lab measurement
(`ml/world_model/demand_study.py`). The estimate is the **instantaneous
runnable demand**: within 0.12 cores of the lab's count of runnable workers
under a binding quota, and within 0.008 cores for always-runnable batch
threads. It is **not** the long-run offered demand of a request server: with
a request backlog it exceeds the offered load by about 1.1 cores, because
every worker stays runnable while the backlog drains. For the 3-second
horizon OSIE predicts, the runnable estimate was the better predictor
(cross-validated error 0.169 against 0.271 for a PSI-based offered-demand
estimate). Kernels whose task-based throttling changes this accounting are
outside the support envelope, and at run time throttled time without
throttled periods, or more throttled CPU-seconds per second than CPUs,
withholds the prediction.

## Safety

- The prediction path has no code that writes to the OS. It reads cgroup and
  CPU counters through `ObservationBroker`; the capability is `observe.*`.
  `EnginePredict.AnswersReadOnlyAndAudits` checks that no `act.*` capability
  is touched.
- **Application latency and throughput are never predicted at run time.** OSIE
  cannot observe them for an arbitrary workload. The lab measured them only for
  instrumented synthetic workloads; see the results document.
- The model file is untrusted input. It has a size cap, strict number parsing,
  a feature-schema check (an unknown feature name rejects the model), shape
  checks, bounded weights and no trailing content after `end`.
  `WorldModelFile.*` has negative tests and a mutation test.
- Output is limited to what was validated. The answer states the model's
  provenance (training environment) and the assumptions it relies on.
  Assumptions include that demand stays as observed and that the mechanism
  edges in the graph hold.

## Model lookup

The lookup is the same as for the intent model:
1. `--world-model PATH`
2. `$OSIE_WORLD_MODEL`
3. `<data dir>/models/world_cpu.osiewm`
4. the model installed with the binary

If none is found, `osie predict` says so and predicts nothing.

## File format (`.osiewm`)

Line-based text, `OSIEWM2` (or the legacy `OSIEWM1`) first and `end` last:

| Key | Meaning |
|---|---|
| `id`, `kind` (`linear`\|`mlp`), `history`, `window_s` | model identity and observation window |
| `features` | names from the encoder schema, in input order |
| `mean`, `scale` | input standardisation |
| `hidden`, `w1`, `b1`, `w2`, `b2` | weights (MLP: ReLU hidden layer; linear: `w2`, `b2` only) |
| `target name unit transform lo hi\|cap` | outputs and their physical bounds (`cap` = capacity after the intervention) |
| `conformal name level half_width` | calibrated intervals |
| `abstain_width name width` | maximum useful interval width |
| `ood_lo`, `ood_hi`, `ood_score_max` | training feature ranges and the distance threshold |
| `action kind lo hi` | interventions and value ranges the model was validated for |
| `provenance text` | training environment, shown to users |
| `residual_on mechanistic` | outputs are corrections to the mechanistic prior |
| *OSIEWM2 only:* | |
| `feature_schema N` | encoder schema version; must equal this build's |
| `model_version`, `data_version`, `calibration_version` | provenance tokens |
| `support_arch A [A...]` | `uname -m` values it was validated on |
| `support_kernel MAJ.MIN MAJ.MIN` | inclusive kernel range |
| `support_cgroup 2` | cgroup version |
| `support_throttle_semantics summed_per_cpu` | the `throttled_usec` accounting the demand estimate relies on |
| `support_capacity lo hi` | CPU capacity after the change, in cores |
| `workload_coverage F [F...]` | workload classes in the training data |
| `target_status name validated\|experimental` | shown with each prediction |
| `residual_gate name max` | optional: apply the residual only in-distribution and when its magnitude is at most `max` |
| `conformal_regime name level lo hi width` | optional: half-width when lo ≤ demand/capacity after the change < hi (`hi` may be `inf`); overrides `conformal` there |
| `safe_throttle_max cores` | threshold of the throttling assessment |

Every OSIEWM2 line except `residual_gate` is required. OSIEWM1 files still
load, but the engine withholds their predictions, because nothing says where
they are valid.

Limits: 4 MiB, 4096 lines, 256 features, 1024 hidden units, 16 targets,
|w| ≤ 10⁶.
