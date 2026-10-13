# Pre-registered decision: model A or model B

Written and committed **before** the GitHub-runner confirmatory data were
downloaded or looked at.

- **A** is the frozen model, `models/world_cpu.osiewm`: sha256 `bd298379…5332`,
  frozen in commit 9002fe6.
- **B** is the candidate `results/v02/world_cpu_v02b_mondrian.osiewm`:
  sha256 `6fdf27fe…328d3`. Its point predictions are identical to A's (same
  mechanism, same PSI residual weights). It adds regime-conditional
  ("Mondrian") interval half-widths for usage, throttled time and the
  throttled-period ratio, conditioned on rho = demand estimate / capacity
  after the change, with edges 0.7 and 1.3. All half-widths come from the
  same 21 calibration runs as A's. Three 95% widths in the rho ≥ 1.3 regime
  use the conservative fallback (17 runs < 19).

**Why B exists.** On the local confirmatory data (now spent as evidence for
this decision), A's 95% throttled-time interval covered only 72% of
outcomes where an unsaturated cgroup became throttled after a cut, and its
throttling assessment never said "low" in 288 changes. On the development
data B fixed the under-coverage (bound regime 0.86 → 0.96). On the local
data it gave 121 "low" assessments with 0 false-safe at the same abstention
rate (15%).

**Rule, applied to the GitHub-runner data with forced predictions** (the
environment check off, because both models' envelope is aarch64/6.10 only):

B replaces A as the shipped model only if all three hold:
1. B's 95% coverage of throttled time in the regime
   `pre_unsaturated/throttled_after` ≥ A's;
2. B's overall 95% coverage of usage and of throttled time ≥ 0.90 in every
   environment;
3. B's false-safe count among "low" assessments is at most
   max(1, 5% of its "low" count) in every environment.

Otherwise A ships unchanged. Both results are reported either way.
