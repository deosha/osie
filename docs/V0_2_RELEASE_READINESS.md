# v0.2 release readiness

The pass/fail evaluation of the v0.2 acceptance gates, with links to the
evidence. "Pass" means it was measured and met. "Pass (CI)" means it is
enforced by a CI job whose result is recorded at the end of this document.

## Recommendation

**Release v0.2.0, with the CPU predictor shipped as an experimental,
opt-in research preview.**

- **Stable:** observation, diagnosis, language understanding and safety
  routing. Every engineering gate passes.
- **CPU prediction:**
  - It is off unless `--experimental` is given.
  - It predicts only inside a declared support envelope.
  - On data collected after the model was frozen, it beat the no-change
    baseline on every target, in every workload group and in every
    environment.
  - It is not "validated" for real services, bare metal, or kernels other
    than the ones measured.
  - CPU-pressure predictions are marked experimental even inside the
    envelope.

The decision on a tag and a GitHub release is the repository owner's; this
document does not create either.

ROUND2_PLACEHOLDER

## Engineering gates

| Gate | Result | Evidence |
|---|---|---|
| All v0.1 tests pass | pass | 237 unit tests on macOS; 242 unit and platform tests on Linux (Docker, kernel 6.10) |
| New language regression tests pass | pass | `SafetyModes.*` (46 cases including a paraphrase property test), `NewIntents.*`, `HypotheticalChange.*`, `Normalize.*` |
| Adversarial action requests are explicitly refused | pass | Frozen test set: 0 of 27 directives misrouted (v0.1: 51.9%). Multi-turn insistence and override: `MultiTurnSafety.*` (10 turns, 10 audited refusals). |
| No mutating capability can execute | pass | No executor exists (`Policy.AllActionsDeniedInV1`). The new `act.file.delete`, `act.network.firewall` and `act.container.control` are declared and denied. `grep -rnE "system\(\|popen\|exec[lv]p?\(" src/` finds nothing. |
| Loader fuzzing and sanitizers pass | pass | `.osiewm` fuzzer: 9.1M runs under ASan + UBSan, no findings. ASan + UBSan, TSan and `-Werror`: 237 tests pass on macOS. The CI `fuzz` job runs both loaders. |
| Missing telemetry is handled | pass | `FaultInjection.*` (8 tests): vanished processes and cgroups, permission errors, missing PSI, corrupt models, 1 MiB input |
| Installed binaries pass clean-environment smoke tests | pass (arm64), pass (CI, x86-64) | `packaging/upgrade_test.sh` on arm64: deb (Ubuntu 22.04), rpm (Fedora 40) and tarball. Each covers a clean install, an upgrade from v0.1.0 and an uninstall leaving no files. CI `packaging` job on x86-64. |
| JSON outputs are schema-valid | pass | `docs/schemas/*.schema.json`; `tests/check_json_schemas.py`: 205 outputs on macOS and the Linux `predict` outputs, 0 violations; runs in CI |
| Release model discovery works | pass | The smoke test checks that both models are found in the installed layout and that `SHA256SUMS` verifies |
| Performance regression within budget | pass | [`BENCHMARKS.md`](BENCHMARKS.md), "v0.2 latency budget": median question 0.53 s against 0.52 s for v0.1, peak RSS +0.3 MB. Prediction p99 1.5 µs (target < 100 µs); rule parsing p99 3.6 µs (target < 1 ms). |

## Predictive-reliability gates

| Gate | Result | Evidence |
|---|---|---|
| Independent model calibration | pass | Separate calibration runs; group-aware split conformal; regime-conditional half-widths |
| The final test data were not used for selection | pass, with one disclosed exception | Model A was frozen before round 1. Model B was frozen with a pre-registered decision rule before the runner data were read; the local round-1 data motivated it. Model C was frozen with a pre-registered rule before round 2. See [`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md). |
| False-safe predictions measured | pass | The assessment uses the 95% upper bound, never the point estimate. Results are reported per environment. The point-estimate rule's false-safe rate is reported for comparison. |
| Problematic workloads fall back or abstain | partial | Out-of-envelope machines: 100% withheld. Unfamiliar families: `wide` 65% and `closed` 42% withheld locally. The `g_host_cpus` defect (W8) made the check withhold *too much* on other hosts; model C fixes it, subject to round 2. |
| Calibration reported by operating regime | pass | Coverage by regime (pre-saturated or not × throttled after or not), and by environment and workload group |
| Unsupported kernels and hardware detected | pass | OSIEWM2 support envelope, checked at prediction time (`WorldModelV2.OutsideTheEnvelopeIsWithheld`) |
| Predictions state assumptions and limitations | pass | The CLI output lists assumptions, the graph mechanisms with their epistemic status, the envelope and target status |
| No application-latency prediction | pass | Never predicted; stated in every answer |
| Experimental prediction is opt-in | pass | `EnginePredict.IsOffUnlessExplicitlyEnabled`; smoke test step 8 |

## Research gates

| Gate | Result | Evidence |
|---|---|---|
| Baselines evaluated | pass | No change; mechanism with two demand estimators |
| Feature and model ablations | pass | Ridge, GBT, MLP 8–64, residual and direct forms, target-specific losses, gates A–F |
| Multiple seeds | pass | 2 seeds in the search, 5 for each finalist; standard deviations reported |
| Independent-run uncertainty | pass | Run-bootstrap CIs; family-bootstrap CIs for language |
| Data-collection cost included | pass | About 92 s per 10-episode run per lane locally; about 50 min per GitHub runner job (27 + 3 runs); 20 min of selection compute |
| Negative results preserved | pass | Learned corrections lost to the mechanism; model A's under-coverage; the W8 defect; post-hoc parser scores kept separate |
| Cross-environment experiments | pass | arm64 and x86-64; Linux 6.10 and 6.17 |
| Unavailable hardware marked incomplete | pass | No bare metal, no kernel < 6.10, no host with more than 4 vCPUs other than the development VM ([`COMPATIBILITY.md`](COMPATIBILITY.md)) |

## Supported environments

See [`COMPATIBILITY.md`](COMPATIBILITY.md).

## Outstanding defects and limitations

- **W8:** host-size feature in model B's out-of-distribution check (see the
  round-2 decision above).
- **W10:** the CPU-pressure residual is clamped at 0 in 12–14% of
  predictions, and its 80% interval under-covers for batch threads.
- **Same-author intent evaluation set.** No utterances from people who have
  not seen the rules.
- **macOS signing.** Binaries carry only the linker's ad-hoc signature. No
  Developer ID identity was available, so nothing was signed or notarised,
  and Gatekeeper may ask for confirmation on first run of a downloaded
  tarball. Homebrew installs are unaffected.
- **Kernel range.** The envelope's kernel range (6.10–6.17 for model C) is
  interpolated: only 6.10 and 6.17 were measured.

## CI

CI_PLACEHOLDER
