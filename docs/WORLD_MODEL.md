# CPU world model

OSIE can answer one kind of hypothetical question on Linux: *what is likely to
happen to a cgroup's CPU usage, throttling and CPU pressure if its CPU quota,
cpuset size or offered load changes?* It answers from a small trained model
and recent observations. **It never applies the change.**

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
| Uncertainty | split-conformal half-widths | 80% and 95% intervals, calibrated on validation runs. |
| Abstention | `WorldModel::predict` | Abstains on any of: an action outside the validated range, a feature outside the training range, an unusual state (mean squared z-score above the validation 99th percentile), missing telemetry, too little history, or an interval too wide to be useful. |
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

Line-based text, `OSIEWM1` first and `end` last:

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

Limits: 4 MiB, 4096 lines, 256 features, 1024 hidden units, 16 targets,
|w| ≤ 10⁶.
