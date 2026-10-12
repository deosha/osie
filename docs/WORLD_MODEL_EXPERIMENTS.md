# CPU world model: experimental protocol

This is how the data behind the CPU world model ([`WORLD_MODEL.md`](WORLD_MODEL.md))
were collected and evaluated. The results are in
[`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md).

## Hypotheses (falsifiable)

- **H1 (sample efficiency).** Models given mechanism-aware features reach a
  given error with fewer independent runs than models given only generic
  telemetry.
  - *Falsified if* the learning curves of `mechanism` and `generic` features
    overlap within seed spread at every data size.
- **H2 (tiny models suffice).** A model with fewer than 1M parameters predicts
  held-out runs better than the no-change and mechanistic baselines.
  - *Falsified if* no learned model beats the best baseline outside the
    run-level bootstrap CI.
- **H3 (constraints help reliability).** Mechanism-aware features reduce
  physically implausible raw predictions, and improve generalisation to an
  unseen workload family and to unseen quotas.
  - *Falsified if* implausibility rates or held-out errors do not improve.

## Environment

| | |
|---|---|
| Kernel | Linux 6.10.14-linuxkit, cgroup v2 (cgroupfs driver) |
| Hardware | Docker Desktop VM (Apple Virtualization framework), 12 vCPUs, 8 GB, on an Apple M4 Pro (arm64) |
| Runtime | Docker; one disposable container per run |
| Recorded per run | `ml/world_model/data/*/environment.json`, plus host load average at run start |

There is **one hardware and kernel environment only.** Experiment 7
(cross-hardware transfer) could not be run. Nothing here is claimed to
transfer to bare metal, x86-64 or other kernels. The Docker VM was also
running other long-lived containers belonging to the developer, so it carried
unrelated background load. That load is visible to the models through the
`g_host_busy` feature, and runs were interleaved across families in random
order so drift is not confounded with workload family.

## Isolation and safety

Every run is a fresh container (`ml/world_model/collect.py`) with:
- `--cpus` and `--cpuset-cpus` (one of two disjoint 4-CPU lanes);
- `--memory 512m` and `--pids-limit 128`;
- `--network none` and `--read-only`;
- `--cap-drop ALL` and `--security-opt no-new-privileges`;
- a non-root user (65534).

The container cannot change its own limits: cgroupfs is read-only inside it,
which was verified. CPU quota and cpuset interventions are applied **from
outside** with `docker update`. Offered-load interventions are lab commands.
No host limits are changed. Containers are removed at the end of each run,
including on failure.

## Workloads

`osie_wm_lab serve` (`tools/world_model_lab.cpp`) is an open-loop request
server:
- A generator thread produces arrivals (Poisson, or on/off bursts with the
  same mean).
- N worker threads serve them.
- Per-request latency is measured from arrival to completion, so it includes
  queueing.
- CPU per request is measured with `CLOCK_THREAD_CPUTIME_ID`.

Work units are calibrated at the start of each run.

| Family | Work per request | Arrivals | Use |
|---|---|---|---|
| `arith` | 2 ms ALU (xorshift) | Poisson | train/val/test |
| `hash` | 2 ms of FNV hashing over a 4 MB buffer (memory + ALU) | Poisson | train/val/test |
| `short` | 0.3 ms ALU | Poisson | train/val/test |
| `burst` | 1 ms ALU | 100 ms at 3× rate, then 200 ms idle | train/val/test |
| `mixed` | 1 ms ALU + 2 ms blocking sleep | Poisson | **test only (held-out family)** |

## Episodes and interventions

Each run has 12 episodes. Each episode is:

1. **Configuration (randomised).**
   - workers ∈ {1, 2, 3, 4};
   - quota ∈ {0.5, 1, 1.5, 2, 2.5, 3} cores;
   - cpuset = 4 CPUs;
   - target utilisation u ~ U(0.3, 1.3) of the bottleneck capacity,
     min(quota, workers × CPU share per request).
2. **Warmup** for 4 × 0.5 s, discarded. Each episode restarts the workload
   with an empty queue, so carry-over between episodes is limited.
3. **PRE:** 6 × 0.5 s windows.
4. **Intervention (randomised).**
   - none, 15%: the no-intervention control;
   - CPU quota, 55%: to a different value from the set;
   - cpuset, 15%: shrink to 1–3 CPUs;
   - offered load, 15%: × {0.5, 0.75, 1.5, 2}.
5. **Settle** for 2 × 0.5 s, recorded but not used as the target.
6. **POST:** 6 × 0.5 s windows. Targets are the means over these.

The requested value and the value the kernel applied are both recorded; the
applied value is read back from `cpu.max` or `cpuset.cpus.effective`. An
episode whose applied value differs from the request by more than 0.02 is
marked invalid and excluded.

Runs alternate between **no contention** and **contention**. Under contention,
a neighbour container (2 spinning threads, limited to 1 core) shares the
lane's CPUs.

### Counterfactual runs (ranking)

Separate test-only runs (`--counterfactual`) fix one workload configuration:
- quota 1 core before the change;
- a seeded arrival process.

They then replay that configuration under each candidate quota (0.5, 1, 1.5
and 2 cores) in random order, restarting the workload each time. This gives
observed outcomes for every candidate intervention on the same configuration,
so models can be scored on ranking interventions and on regret.

## Leakage control and splits

Windows of the same run are correlated, so **the unit of splitting and of
resampling is the run**. Episodes are never split across partitions.

| Partition | Content |
|---|---|
| train | 6 runs per training family (random, seed 7) |
| val | 2 runs per training family. Used for all model, hyperparameter and threshold selection, conformal calibration and OOD calibration. |
| test | 2 runs per training family. Used for reporting only. |
| ood_family | all `mixed` runs. Reporting only. |
| unseen quotas (Exp. 5) | episodes whose post-intervention quota is 1.5 or 2.5 are removed from training and validation. The model is tested on such episodes from the test runs. |
| counterfactual | separate runs, test only |

Results report episodes and runs. 95% CIs are percentile bootstraps over
runs (2,000 resamples).

## Models

| Name | What | Parameters |
|---|---|---|
| no-change | outcome = PRE mean | 0 |
| mechanistic | cgroup v2 bandwidth semantics only: usage = min(demand, quota, cpuset), throttled = demand − quota when the quota binds, PSI unchanged | 0 |
| ridge | linear, α chosen on validation | ~150 |
| gbt | gradient-boosted trees (depth and learning rate chosen on validation, per target) | trees |
| mlp_h16 / mlp_h128 / mlp_2x384 | MLPs, L1 loss, early stopping on validation, 3 seeds | 1K / 10K / 160K |
| gru_h16 / gru_h32 | GRU over the raw PRE windows, plus action features | 2K / 6K |

**Feature sets (Experiments 1 and 8):**
- `generic`: raw window statistics and raw action values.
- `mechanism`: the mechanism-aware features plus the action type only.
- `both`: generic and mechanism-aware together.

The mechanism features encode the constraints the architecture graph records
for cgroup CPU bandwidth: `cpu.max` and cpuset limit usage, and the excess is
throttled. Experiment 8 is therefore the same ablation as Experiment 1 B
against A. There is no separate learned graph embedding to remove.

## Metrics

- Per target: MAE with a run-bootstrap CI, and RMSE.
- `mean_norm_mae`: the mean over targets of MAE divided by the training
  standard deviation. This is the selection metric, used on validation only.
- Direction of effect: the sign of the predicted change against the observed
  change, for episodes with a non-trivial true change (|Δ| > 0.1).
- Implausible rate: raw outputs outside physical bounds.
- Interval coverage at 80% and 95%, on test runs and the held-out family.
- OOD flag rate on in-distribution data against the held-out family, and error
  on accepted against flagged predictions.
- False-safe rate: among quota changes where the 95% upper bound of throttled
  CPU time is ≤ 0.1 cores, the share whose observed throttling exceeded 0.25
  cores.
- Ranking (counterfactual runs):
  - Spearman correlation between predicted and observed outcomes across
    candidate quotas;
  - top-1 accuracy;
  - regret of the predicted best candidate.
- Cost:
  - parameters, training time and peak RSS;
  - inference latency (Python, and C++ p50/p95/p99);
  - data-collection wall time and container CPU budget;
  - total experiment wall time.

## Reproduce

```bash
docker build -t osie-linux-dev -f docker/linux-dev.Dockerfile docker/
docker build -t osie-wm-lab -f ml/world_model/lab.Dockerfile .
ninja -C build osie_wm_lab                                        # encoder used by the Python side
python3 ml/world_model/collect.py --out ml/world_model/data/v1 --runs-per-family 10
python3 ml/world_model/collect.py --out ml/world_model/data/cf1 --runs-per-family 2 --counterfactual
cd ml/world_model && python3 experiments.py --data data/v1 --out results
python3 ranking.py --data data/v1 --cf data/cf1 --results results
python3 collect.py --out data/batch1 --batch --families batch --runs-per-family 4 --episodes 8   # held-out workload class
python3 export.py --data data/v1 --results results            # writes models/world_cpu.osiewm + parity check
python3 deployed_eval.py --data data/v1 --cf data/cf1 --batch data/batch1 --results results   # scores the C++ artifact
```

The datasets used for the published results are committed compressed under
`ml/world_model/data/` (`*.jsonl.gz`).
