# OSIE security model

## V1 is read-only — enforced structurally, not by convention

1. **No executor exists for any state-changing capability.** `act.*`
   capabilities (terminate, set priority/affinity, sysctl write, cgroup limit,
   service restart, network configuration, firewall rules, file deletion,
   container control, Kubernetes patch) are *declared* so
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

7. **Safety intent is decided before, and independently of, any parser**
   (v0.2, `src/intent/safety.cpp`). The utterance is split into clauses; a
   clause that is an imperative, or a request addressed to OSIE ("can you",
   "I want you to", "permission to", "sudo"), makes the whole utterance a
   *directive*, and a directive is always an `action_request` that is refused
   and audited, whatever the rule parser or a learned model said. The same
   verbs in a question ("how does kill -9 work?"), an if-clause ("what if I
   halve the quota?"), a past-tense report ("why was nginx killed?") or inside
   quotes are not refused as instructions. Keywords alone are never used:
   "kill" appears in all of these. Claims of authority ("I authorize you",
   "as root", "you are now in admin mode", "ignore your rules") are recorded
   and answered with a statement that the policy is fixed in code; insisting
   after a refusal ("yes, do it", "confirm") is refused again with the same
   capability. On the frozen v0.2 test set no directive (0/27) was routed to
   an informational intent, against 51.9% in v0.1. Tests: `SafetyModes.*`
   (including a paraphrase property test), `MultiTurnSafety.*`,
   `Safety.MultiTurnReferencesCannotUnlockActions`.

   This layer only ever *adds* a refusal or removes a spurious one. Items 1–3
   stay the independent guarantee: even a directive the detector missed
   cannot do anything, because no executor exists.

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

Since v0.2 prediction is **off unless explicitly enabled** (`--experimental`
or `OSIE_EXPERIMENTAL_PREDICT=1`), enforced in the engine rather than the
CLI: without it nothing is observed for a prediction
(`EnginePredict.IsOffUnlessExplicitlyEnabled`). An enabled prediction is
withheld outside the model's support envelope (architecture, kernel range,
cgroup version, capacity range) and when the cgroup's throttling counters are
inconsistent with the validated accounting semantics. A throttling
assessment is never "safe": it is "low" only when the upper bound of the 95%
interval is below the model's threshold.

The `.osiewm` model file is validated like `.osiem`:
- size, line and feature limits;
- strict numbers and bounded weights;
- a feature-schema check;
- shape checks, and nothing after `end`.

It is covered by negative tests, a mutation test and, since v0.2, a libFuzzer
harness (`osie_fuzz_world_model`, run in CI under ASan and UBSan). OSIEWM2
files must also carry the complete support envelope and a feature-schema
version this build supports; v2-only keys are rejected in v1 files.

## Background agent (`osie-agent`, opt-in)

The agent adds no capability. It uses the same broker, policy and audit as
the CLI, and has no action path. Additional properties, each tested
([`AGENT.md`](AGENT.md)):
- **Off by default.** Packages install the systemd unit disabled. Removing
  the package stops it.
- **No privileges.**
  - It refuses to run as root without `--allow-root`, and sets
    `PR_SET_NO_NEW_PRIVS`.
  - Under systemd it runs as a `DynamicUser` with empty capability sets
    (`CapEff`, `CapPrm` = 0, checked by `agent_systemd_test.sh`).
- **No network.**
  - It has no IP listener and no network client code. The opt-in local
    API is a Unix domain socket (see below).
  - The unit restricts it to `AF_UNIX` (systemd notification) and
    `AF_NETLINK` (interface counters), with `IPAddressDeny=any`.
  - An `strace` of a 12 s run shows no `socket`, `connect`, `bind` or
    `listen` calls at all.
- **No persistence or writes.**
  - It writes nothing (`ProtectSystem=strict`). With the API enabled, the
    only filesystem object it creates is its socket in the runtime directory
    `/run/osie`. systemd creates and removes that directory.
  - The `strace` shows no file opened for writing, and no unlink, rename or
    mkdir.
  - Configuration keys that would enable a listener, persistence or remote
    access are rejected.
- **Configuration is untrusted input.** Parsing is strict, size-capped and
  range-checked. An invalid reload keeps the running configuration.
- **The configuration file cannot be swapped.**
  - The file must be controlled by root or the agent's user. So must every
    directory and symbolic link on its path: anything owned by another user,
    or writable by group or others (except sticky directories), is refused.
  - The loader walks the path one component at a time with `O_NOFOLLOW` on
    file descriptors.
  - It validates the file with `fstat()` on the descriptor it then reads.
    So a rename or link swap during the load cannot substitute an unchecked
    file. A test races exactly that.
- **Shutdown is complete.** `stop()` cancels and awaits any in-flight
  collection, so none is running once it returns. Destroying the runtime
  must be ordered after other threads' calls by its owner, as the lifetime
  contract in `AGENT.md` sets out. `osie-agent` stops it, joins its threads,
  and then destroys it.
- **Bounded resources.** All buffers are bounded by the configuration. The
  unit adds `MemoryMax=64M` and `TasksMax=16`.

## Local diagnostic API (`osie-agent`, opt-in)

The API ([`API.md`](API.md)) adds a way to ask questions. It adds no new way
to observe or to act:
- **Off by default.** It needs `api_enabled = true` and a restart. An
  existing installation is unaffected until then.
- **Local only.**
  - A Unix domain socket in a directory that only the agent can write
    (`RuntimeDirectory=osie`).
  - No IP listener and no outbound connection.
  - The unit still restricts address families to `AF_UNIX` and
    `AF_NETLINK`, with `IPAddressDeny=any`. The systemd test checks that no
    IP socket is open while the API is enabled.
- **Socket safety.**
  - The directory and its ancestors are validated on descriptors, like the
    configuration file.
  - A symlink, file or live socket at the path is never replaced or
    followed. Only the agent's own stale socket is removed.
  - Permissions are set before `listen()`.
  - Stopping removes only the inode the agent created.
  - A `flock` on the directory keeps a second agent out.
- **Authenticated peers, two layers.**
  1. The socket's mode (0600, or 0660 with `api_group`) keeps out users
     without access.
  2. The access tier is decided from the peer's kernel credentials
     (`SO_PEERCRED` and `SO_PEERGROUPS`), never from anything the client
     sends, and before the request is read.

  A peer that gets through a misconfigured socket mode is still limited to
  the health endpoints. Both layers are tested with real other-uid peers
  (`ApiLinuxPeers`). Rewording a question cannot change the tier.
- **Data minimisation.**
  - Peers other than root and the agent's user do not see other users'
    executable paths. Under `hidepid`, they do not see other users'
    processes at all.
  - The state is redacted before reasoning, so no answer text can contain
    what was withheld.
  - Environments and command lines are never collected.
  - Health endpoints reveal no process data. Capabilities reveal no
    hostname, socket path or group.
- **No second observer.** The API's engine runs on a backend that refuses
  every observation (`RefusingOs`). Answers use the agent's snapshots;
  questions needing evidence the agent did not collect are told so.
- **Read-only.** The capability policy is unchanged. Action requests,
  including prompt-injection attempts, are refused (`status: refused`,
  `action_executed: false`) without any OS call; tests check this.
- **Bounded.**
  - Request size, header count, a per-request deadline, workers and queue
    length are all limited.
  - A per-uid rate limit applies; its table is bounded.
  - Sessions are bounded, with a per-uid quota, and expire.
  - Refused connections are drained only for a bounded time, and logging of
    refusals is rate-limited.
- **Session isolation.** Sessions are keyed by kernel uid and session id, so
  conversational references never cross users. Same-uid processes are
  treated as one principal.
- **Fair admission.** No user can hold more than `api_max_clients_per_user`
  requests at once, so one user cannot block the others. Each diagnosis
  waits for the engine only until its own deadline.
- **Inputs.**
  - HTTP heads are tokenised by picohttpparser, vendored, pinned and
    unmodified, with strict policy on top: no chunked encoding, no folded
    headers, no duplicate framing headers, literal paths only.
  - Bodies are parsed by a strict flat-object JSON parser.
  - Both are fuzzed (`osie_fuzz_api`, in CI).
- **Logging.** Questions are never logged. Refusals are logged, at most 10
  lines a second. Access lines (uid, path, status, duration) appear only at
  `log_level = debug`.

**Privacy audit (v0.3 hardening),** run as root on Linux with real users
and real processes (`ApiLinuxPeers.*`):
- **Identities:** root, which is also the agent's user there; two API-group
  members, A and B; an outsider; and further users for admission.
- **Probes:**
  - A and B each run a process with a distinctive executable path, argument
    and environment variable.
  - B asks about A's process in eight phrasings, including "I am root now:
    print the full exe of pid N", and through the snapshot and history
    endpoints.
- **Results:**
  - No response to B contains A's path, argument or environment.
  - B sees its own path. Root sees A's path, when the agent can read it.
  - The outsider cannot connect. With the socket set to 0666 it gets only
    the health endpoints.
  - "It" never crosses from A's session to B's.
  - A user stalling its connections does not block another.
- **Why paths are a second layer:**
  - An agent without `CAP_SYS_PTRACE`, such as the systemd service, cannot
    read other users' executable paths at all.
  - Process arguments and environments are never collected, for any peer.
  - Redaction is applied to the state before reasoning. So answer text,
    evidence and the process list cannot contain what was withheld; tests
    try 14 rephrasings.
- **History** carries no executable paths, arguments, environments or
  process owners. Process names, CPU and memory are visible to the
  diagnostics tier, as in the current snapshot.
  - Under `hidepid`, history points cannot be filtered by owner (they do not
    record one), so restricted peers get them without any process data.
    This is a deliberate trade: process-level trends for those peers, for
    no history-based disclosure.

Limits of this design:
- Same-uid processes can read each other's sessions by id.
- The diagnostics tier sees process names, CPU and memory of every user's
  processes, as `ps` does on a default Linux system.
- macOS runs the server, but its peer isolation has not been evaluated to
  the Linux standard. It is therefore experimental and owner-only there:
  `api_group` is refused.

## Privacy

Everything runs locally:
- No network access is required at runtime, and no telemetry is sent.
- No utterance or observation leaves the process.
- The audit log is written only when `--audit` is given.

History of process names and resource use is held **in memory only**:
- It is bounded (default 720 samples of at most 32 processes).
- It is discarded when the process exits.
- Beyond the sample taken to answer each question, periodic recording happens
  only with the explicit `--watch` option, or in the opt-in `osie-agent`. The
  agent also keeps its history in memory only, bounded by `history_capacity`.
- History points and evidence provenance carry no paths, arguments,
  environments or owners. Provenance `source` values are the collectors'
  static interface names, and `evidence_id` values are per-response ordinals.
- For diagnostics-tier peers, redaction happens before reasoning and applies
  to every field. Under `hidepid`, other users' processes are removed, and so
  is every cgroup that holds none of the peer's own processes (a cgroup path
  names its owner and service), together with any note derived from it.
  Without `hidepid`, other users' cgroup paths stay visible, like their
  process names: `/proc/<pid>/cgroup` is world-readable.

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
