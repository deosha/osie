# OSIE security model

## V1 is read-only — enforced structurally, not by convention

1. **No executor exists for any state-changing capability.** `act.*`
   capabilities (terminate, set priority/affinity, sysctl write, cgroup limit,
   service restart, network configuration, Kubernetes patch) are *declared* so
   requests can be recognised, explained and refused, but there is no code
   path that performs them (`CapabilityDescriptor::implemented == false`,
   asserted by `Policy.AllActionsDeniedInV1`).
2. **`SafetyPolicy::authorize` denies any capability whose kind is `act` or
   that declares side effects**, regardless of configuration — even
   `PolicyConfig{read_only=false}` cannot enable actions.
3. **All OS access goes through `ObservationBroker`**, which maps each
   observation kind to a registered capability, asks the policy and records
   the decision in the `AuditLog` (bounded ring buffer + optional JSON-lines
   file via `--audit FILE`). An unregistered observation is denied.
4. **No shell.** OSIE never spawns processes (`ps`, `top`, `sysctl`, …) and
   contains no `system()`/`popen()`/`exec*()` call. Verify with
   `grep -rnE "system\(|popen|exec[lv]p?\(" src/` (no matches).
5. **No privilege escalation.** OSIE does not request root, entitlements,
   `task_for_pid`, `CAP_SYS_PTRACE` or TCC permissions. Data it cannot read is
   reported as unavailable (`Validity::partial` / `Errc::permission_denied`),
   never estimated.
6. **The model never chooses privileges or observations.** A parser outputs
   only an intent label, a confidence and token spans. The required
   observations and capability ids are derived from the intent by a fixed
   table in `build_frame()`, then schema-validated (`validate()` rejects
   `read_only=false`, non-`observe.*` capabilities on read intents, malformed
   PIDs, out-of-range `top_n`, …).

## Capability descriptor contract

Every capability declares: id, description, kind, risk, platforms, required
permissions, confirmation requirement, reversibility, side effects,
preconditions, postconditions, rollback feasibility, observation kinds.
`validate()` rejects inconsistent descriptors (an observe capability with
side effects or risk > low; a medium+ risk action without confirmation; an
irreversible action without documented rollback infeasibility; a missing
pre/postcondition), so future actions cannot be registered carelessly.

## Input handling

* Utterances are tokenized; text is never interpolated into paths or commands.
* PIDs are parsed as digits only (≤ 9 digits); names are matched against the
  in-memory process list, never used to open files.
* cgroup paths come from `/proc/<pid>/cgroup`, but `LinuxOperatingSystem::cgroup()`
  still rejects empty, relative, `..`-containing or NUL-containing paths
  (`LinuxFixture.CgroupThrottlingAndPsiFromFixtureTree`).
* `/proc/<pid>/stat` parsing uses the last `)` so hostile `comm` values
  (spaces, parentheses) cannot shift fields (`LinuxParse.ProcStatWithHostileComm`).
* **Model files are untrusted input.** The loader (`IntentModel::load`)
  enforces the limits and rules in [`MODEL_FORMAT.md`](MODEL_FORMAT.md):
  * A size cap, checked before the file is read.
  * Strict number parsing.
  * Checked arithmetic for shape products and offsets, so nothing is
    allocated before its bytes are known to exist.
  * No duplicate keys, tensors or labels.
  * Labels restricted to `[A-Za-z0-9_-]`.
  * No trailing bytes or unused tensors.
  * Every weight finite and bounded (|w| ≤ 1e6), so the forward pass cannot
    overflow.

  A malformed file fails with an error and never crashes the process. Coverage:
  * `InferenceHardening.*`, including a deterministic mutation test;
  * a libFuzzer harness (`tests/fuzz/`) that runs in CI under ASan and UBSan.

  Fuzzing has already found one defect: finite weights near `FLT_MAX`
  produced NaN probabilities.
* **Parser output is untrusted.** Before a frame is built, an intent outside
  the enum becomes `unsupported` and a non-finite confidence becomes 0. A
  frame that fails validation is refused before anything is observed.
  Action requests are always refused and audited, whatever the target
  (`Safety.*` tests).
* **Model lookup never uses the working directory.** A model file in an
  arbitrary directory cannot change how questions are interpreted. An
  explicitly requested model that fails to load is an error, not a silent
  fallback.

## Memory/UB safety

The full unit and platform suites pass under AddressSanitizer +
UndefinedBehaviorSanitizer and ThreadSanitizer (see `docs/BENCHMARKS.md` for
commands). UBSan found and we fixed one real bug (misaligned `if_msghdr2`
access on macOS). Sanitizer builds compile GoogleTest from source
(pinned SHA-256) to avoid false positives from an uninstrumented prebuilt gtest.

## Prediction (Linux)

`osie predict` is registered as `observe.predict.cpu_world_model`:
- kind `observe`, no side effects;
- policy-checked and audited.

It reads cgroup and CPU counters through `ObservationBroker` and evaluates a
model. There is no code path that writes `cpu.max`, `cpuset` or any other
control file, and `act.cgroup.set_cpu_limit` remains declared-but-denied.

The `.osiewm` model file is validated like `.osiem`:
- size, line and feature limits;
- strict numbers and bounded weights;
- a feature-schema check;
- shape checks, and nothing after `end`.

It is covered by negative tests and a mutation test.

## Privacy

Everything runs locally:
- No network access is required at runtime, and no telemetry is sent.
- No utterance or observation leaves the process.
- The audit log is written only when `--audit` is given.

History of process names and resource use is held **in memory only**:
- It is bounded (default 720 samples of at most 32 processes).
- It is discarded when the process exits.
- Beyond the sample taken to answer each question, periodic recording happens
  only with the explicit `--watch` option.

## Reporting a vulnerability

Please report security issues privately via GitHub's "Report a vulnerability"
(Security tab of the repository) rather than in a public issue. Useful
reports include the OSIE version (`osie version`), the platform and a
minimal reproduction, for example a model file or an utterance.

In scope:
- a way to make OSIE change system state;
- a way to read data it should report as unavailable;
- a crash or memory error on malformed input (model files, `/proc`
  contents, utterances);
- model lookup loading a file the user did not choose.

## Future actions (not implemented)

The path to enabling an action would require: an executor bound to a single
capability id; precondition checks against fresh observations (identity by
`(pid, start_time)`); explicit user confirmation for medium+ risk; dry-run
output; post-condition verification; rollback where feasible; and an audit
record containing before/after state. Predictive models may *propose*
actions; they must never authorise them.
