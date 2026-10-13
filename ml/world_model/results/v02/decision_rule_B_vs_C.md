# Pre-registered decision: model B or model C

Written and committed **before** the round-2 confirmatory data were collected.

- **B** is the shipped model: sha256 `6fdf27fe…328d3`.
- **C** is `results/v02/world_cpu_v02c.osiewm`: sha256 `568aaaa7…906aa`,
  frozen by `v02_freeze_c.py`. Its design is B's, with no new selection.
  `g_host_cpus` is dropped from the inputs and the range check (audit W8),
  and C is fitted and calibrated on the development data plus confirmatory
  round 1. Its envelope is aarch64 and x86_64, Linux 6.10–6.17. Only 6.10
  and 6.17 were ever observed; the kernels in between are assumed.

**Round-2 confirmatory data:** new seeds, new run ids (`r2-`), the same nine
families and the batch class:
- local Docker Desktop VM (6.10, aarch64), seed 999 (batch 1000);
- GitHub-hosted x86-64 and arm64 runners, seeds 5151 (batch 5252).

**Rule.** C replaces B only if every condition holds in every environment
(`v02_confirm.py`):
1. C's *deployed* (environment check on) withheld rate on `held_in`
   workloads is ≤ 25%;
2. C's 95% coverage of usage and of throttled time is ≥ 0.90 (forced
   predictions, all groups);
3. C's deployed false-safe count among "low" assessments is
   ≤ max(1, 5% of its "low" count);
4. C's usage MAE (forced) is below the no-change MAE in every workload
   group.

Otherwise B stays. Both results are reported either way.
