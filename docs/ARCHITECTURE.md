# OSIE architecture

OSIE turns a natural-language question about the local machine into a *typed*,
*validated* request, gathers only the native observations that request needs
(through a policy-checked broker), updates an explicit OS architecture graph,
runs deterministic reasoning that separates observation from hypothesis, and
renders a grounded answer. A neural model is optional and only ever touches
the first step.

```text
 utterance
    │  tokenize (shared C++/Python tokenizer)
    │  normalize ── abbreviations, bounded typo correction towards cue words only
    ▼
 assess_safety ── clause-level: directive | explanation | past_event | hypothetical | quoted_mention
    │             (decided before, and independently of, any parser)
    ▼
 IIntentParser ── RuleParser (0 params) │ NeuralParser (.osiem) │ HybridParser
    │  ParseOutput {intent, confidence, typed spans}
    ▼
 build_frame()  ── safety routing (a directive is always an action_request);
    │              deterministic: target, entities, REQUIRED OBSERVATIONS,
    │              capability ids, unavailable observations (never chosen by a model)
 SessionContext ── follow-ups ("what about memory?"), references ("it", "the second one")
    │              resolved against *current* state with (pid, start_time) identity
 validate()     ── schema checks (read_only, target types, ranges, capability kinds)
    ▼
 StateSampler ──► ObservationBroker ──► SafetyPolicy + AuditLog ──► IOperatingSystem
    │   two samples Δt apart; rates are Validity::derived;            ├─ MacOperatingSystem (Mach, libproc, sysctl, NSProcessInfo)
    │   each failure recorded with its Errc reason                    └─ LinuxOperatingSystem (procfs, sysfs, cgroup v2, PSI, rtnetlink)
    ▼
 StateCache (shared_mutex; freshness budget)  ──►  KnowledgeGraph
    │                                                 ontology (concepts + documented relations)
    ▼                                                 + instance layer (host, CPUs, processes, users, cgroups, services …)
 Reasoner  ── findings typed as observation / hypothesis / correlation /
    │         causal_explanation / suggested_investigation / proposed_optimization,
    │         each with evidence ids and ontology mechanisms (edge nature + doc ref)
    ▼
 render ── text or JSON (`--json`)
```

## Modules

| Module | Path | Responsibility |
|---|---|---|
| core | `include/osie/core`, `src/core` | `Result<T>`/`Errc`, `Observation<T>` + `Provenance` (source, wall/mono time, validity), cancellation, JSON writer |
| platform | `include/osie/platform`, `src/platform/{macos,linux}` | `IOperatingSystem` (shared concepts only) + typed `IMacExtensions` / `ILinuxExtensions` |
| capabilities | `include/osie/capabilities`, `src/capabilities` | registry, descriptor validation, `SafetyPolicy`, `AuditLog`, `ObservationBroker` |
| state | `include/osie/state`, `src/state` | raw counters → rates (pure functions), `StateSampler`, `StateCache`, bounded `StateHistory` + trend statistics |
| architecture | `include/osie/architecture`, `src/architecture` | typed knowledge graph, ontology, incremental instance updates |
| reasoning | `include/osie/reasoning`, `src/reasoning` | CPU, memory, slowness, per-process and concept diagnoses |
| intent | `include/osie/intent`, `src/intent` | tokenizer, normaliser, clause-level safety assessment (`safety.cpp`), the staged pipeline `understand()`, `IntentFrame` schema, frame builder, validation, rule parser |
| inference | `include/osie/inference`, `src/inference` | hardened `.osiem` loader ([MODEL_FORMAT.md](MODEL_FORMAT.md)), embedding-bag/BiGRU/CNN forward pass, hybrid parser, model locator |
| prediction | `include/osie/prediction`, `src/prediction` | CPU world model: cgroup windows, the shared feature encoder, the mechanistic prior, the `.osiewm` loader, bounded and conformal predictions, abstention ([WORLD_MODEL.md](WORLD_MODEL.md)) |
| engine | `include/osie/engine`, `src/engine` | orchestration, session context, rendering |
| agent | `include/osie/agent`, `src/agent` | opt-in background runtime: config, scheduler, snapshots, health ([AGENT.md](AGENT.md)); `osie-agent` executable |
| api | `include/osie/api`, `src/api`, `third_party/picohttpparser` | opt-in local diagnostic API ([API.md](API.md)): HTTP framing on picohttpparser, strict JSON input, peer credentials and access tiers, redaction, sessions, `AgentEvidence` and `RefusingOs`, the socket server |
| cli | `src/cli` | `osie` REPL / `ask` / `parse` / `snapshot` / `caps` / `graph` / `version`; opt-in `--watch` sampler |
| tools | `tools/` | `osie_eval` (E2E evaluation), `osie_ort_bench`, `osie_cpu_lab` and `osie_wm_lab` (Linux experiments, world-model encoding, parity and benchmarks) |

## Platform contracts

`IOperatingSystem` exposes only concepts that genuinely exist on XNU and Linux:
system info, CPU topology, cumulative CPU ticks, VM statistics, processes,
filesystems, network interfaces. Platform-only facilities are **not** forced
into it:

* `IMacExtensions::power_state()` — `NSProcessInfo.thermalState` and Low Power
  Mode (qualitative; macOS exposes no public temperature API).
* `ILinuxExtensions::pressure()`, `cgroup()`, `cgroup_of()` — PSI and cgroup v2.

Fields a platform cannot provide are `std::nullopt`, never zero: e.g.
`MemoryMetrics::compressed_bytes` (macOS only), `pressure_level` (macOS only),
`CpuSample::runnable_tasks`/`iowait` (Linux only). `supported()` is computed by
*probing* at start-up (PSI can be compiled in but disabled with `psi=0`).

### macOS adapter (verified on macOS 15.7, Apple M4 Pro)

| Observation | Interface |
|---|---|
| CPU ticks per core | `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` |
| VM statistics | `host_statistics64(HOST_VM_INFO64)`, `vm.swapusage`, `kern.memorystatus_vm_pressure_level` |
| topology | `hw.physicalcpu`, `hw.logicalcpu`, `hw.nperflevels`, `hw.perflevelN.*` |
| processes | `proc_listallpids`, `PROC_PIDT_SHORTBSDINFO`, `PROC_PIDTASKALLINFO`, `proc_pid_rusage(RUSAGE_INFO_V4)`, `proc_pidpath` |
| filesystems | `getfsstat(MNT_NOWAIT)` |
| network | `sysctl(PF_ROUTE, NET_RT_IFLIST2)` (64-bit `if_data64` counters) + `getifaddrs` |
| thermal | `NSProcessInfo` via Objective-C++ (`native_bridge.mm`) |

Facts established empirically and encoded in the adapter (each covered by a test):

* `pti_total_user/system` and `ri_*_time` are **Mach absolute-time units** on
  Apple Silicon (timebase 125/3), not nanoseconds; they are converted with
  `mach_timebase_info`.
* Unprivileged, `PROC_PIDT_SHORTBSDINFO` succeeds for every process but CPU/memory
  flavours fail with `EPERM` for other users' processes (191 of 725 at the time
  of writing). Those processes are `Validity::partial` with empty CPU/memory.
* `if_msghdr2` records in the routing sysctl buffer are not 8-byte aligned;
  UBSan caught a misaligned 64-bit load, fixed by copying into aligned locals.
* Kernel names are truncated to 32 bytes; the full name is recovered from the
  executable path when it is a prefix match.
* There is no public API mapping a PID to its launchd job label; OSIE reports
  "launchd (direct child)" and says the label is unavailable.

### Linux adapter (verified on Ubuntu 24.04, kernel 6.10 linuxkit, in Docker)

| Observation | Interface |
|---|---|
| CPU ticks, runnable/blocked, ctxt | `/proc/stat` |
| load | `/proc/loadavg` |
| memory | `/proc/meminfo` (`MemAvailable`), `/proc/vmstat` (`pswpin/out`, `pgfault`, `pgmajfault`) |
| processes | `/proc/<pid>/{stat,status,cgroup,exe}` (comm parsed with the *last* `)`) |
| topology | `/sys/devices/system/cpu/*/topology`, cache `index*`, `sched_getaffinity` |
| PSI | `/proc/pressure/{cpu,memory,io}` and per-cgroup `*.pressure` |
| cgroup v2 | mount from `/proc/self/mountinfo`; `cpu.max`, `cpu.stat`, `memory.*`, `pids.current` |
| service ownership | systemd unit inferred from the cgroup path (`EdgeNature::inferred`) |
| network | rtnetlink `RTM_GETLINK` dump with `IFLA_STATS64`; `/proc/net/dev` fallback |
| filesystems | `/proc/self/mounts` + `statvfs` |

`pgpgin/pgpgout` count KiB of block I/O, so they are deliberately **not** mapped
to page-ins/page-outs. The adapter accepts a `root` prefix so tests can run it
against a fixture `/proc` + `/sys/fs/cgroup` tree.

## Observations, freshness and identity

Every value is an `Observation<T>{value, Provenance{source, wall, mono, validity, note}}`.
Rates come from two samples `interval` apart (default 500 ms) and are marked
`derived`; counters that go backwards (reset, hot-unplug, wrap) yield *no*
rate rather than a negative one. The `StateCache` reuses a state only if it
is younger than the freshness budget (2 s) **and** contains every observation
the new question needs (or recorded why it is unavailable).

Processes are identified by `(pid, start_time)`. PID reuse produces a new
graph node. A reference to an exited process gets a clarification ("… is no
longer running"), never a silent answer about a different process.

`SessionContext::resolve` returns a `Resolution` and applies these rules:

- **Same name.** A name that matches several processes is never collapsed
  silently to one of them. Exact matches form the group; processes whose name
  merely contains the query are counted and mentioned, but not included.
  - CPU and memory questions get the group's total, the instances ranked by
    the metric asked about, and an analysis explicitly scoped to the top pid.
  - Single-process questions (info, tree) list the candidates and ask which
    one.
  - Ordinals in the next turn refer to the list just shown.
- **Reused PIDs.** When the platform does not report a start time, the name
  is compared instead, and a mismatch is treated as PID reuse.
- **Expiry.** References ("it", "the second one") expire after
  `EngineConfig::context_ttl` (10 min). After that OSIE asks again instead
  of assuming.
- **Out-of-range ordinals.** An ordinal beyond the last list gets a question,
  not a default.

## Effective CPU capacity (v0.2)

"How many CPUs" has five different answers on Linux, and OSIE never
substitutes one for another. `CpuCapacity` (`include/osie/platform/types.hpp`)
holds each limit with its status (`known`, `unlimited`, `unknown`,
`not_applicable`) and the interface it came from:

| Limit | Meaning | Source |
|---|---|---|
| host logical CPUs | CPUs the kernel knows | `sysconf(_SC_NPROCESSORS_CONF)` |
| online CPUs | CPUs currently online | `/sys/devices/system/cpu/online` |
| affinity | *where* the process may run | `sched_getaffinity(2)` (dynamically sized mask) |
| cgroup cpuset | CPUs granted to the cgroup | `cpuset.cpus.effective` |
| cgroup quota | *how much CPU time* per period | minimum of `cpu.max` over the cgroup and every visible ancestor |

`finalize_capacity()` takes the minimum over the known limits and names the
binding one; an unknown limit is listed as "could not be measured, so the
true limit may be lower", never defaulted. On macOS the cgroup and affinity
limits are `not_applicable`. Inside a cgroup namespace the limits above the
namespace root are invisible, and the answer says so.

The CPU diagnosis uses this distinction: a cgroup using ≥ 90% of its quota
and throttled in most periods is reported as quota-bound even when the host's
CPUs are mostly idle. Throttling evidence is a rate over the sampling window
(OSIE's own cgroup is read at both ends of the window; for cgroup questions
every inspected cgroup is), and a lifetime ratio is labelled as one.

## Evidence sources: the CLI and the agent API (v0.3)

`Engine::ask()` is one pipeline whatever the evidence: normalise, assess
safety, parse, build and validate the frame, apply session context, resolve
targets, then reason and render. Two things are parameters of a call:
- **The session** (`SessionContext&`). The CLI uses the engine's own session.
  The agent API passes the session of the asking peer (uid, session id).
- **The evidence** (`EvidenceSource&`), which supplies the state an intent
  needs and the history temporal questions read.

| Source | Used by | Where states come from | History |
|---|---|---|---|
| `Engine::LiveEvidence` (internal) | `osie ask`, the REPL | the sampler, a 500 ms window, reusing a cached state younger than the freshness budget | the engine's own `StateHistory` |
| `api::AgentEvidence` | the local API (`/v1/diagnose`) | the agent's newest published snapshot, refused if older than the evidence-age limit; cgroup throttling rates from the agent's last two snapshots | a copy of the agent's history |

The API's engine is built on `api::RefusingOs`:
- It reports the agent's platform and supported observation kinds, and
  refuses every observation call.
- Any path that would observe on demand, such as the capacity read, fails
  with an explicit reason, which the API reports as a limitation.
- Tests assert that answers covered by the snapshot make no such call. So
  there is one sampler, one scheduler and one history: the agent's.

What the API adds around the pipeline:
- authorisation from the peer's kernel credentials;
- redaction of the state for non-full peers, before reasoning;
- request validation and limits;
- a versioned JSON envelope. Its evidence items, the diagnosis object and the
  trends are written by the same serialisers as `osie ask --json`
  (`include/osie/engine/serialize.hpp`).

## Temporal state

`StateHistory` (`include/osie/state/history.hpp`) is a bounded ring buffer:
by default 720 points.

**What a point holds:**
- machine CPU busy and memory in use;
- memory pressure: the macOS kernel level, or Linux PSI avg10;
- the top 16 processes by CPU and by memory, keyed by (pid, start time);
- the conversation's current subject, which is always kept.

**When it records.** There is no daemon. Points are recorded in three cases:
- whenever OSIE samples to answer a question;
- every N seconds with the opt-in `--watch N`, while a session is open,
  using a thread that serialises with `ask()` through the engine mutex;
- before answering, with `ask --for S`.

Nothing is written to disk.

**How temporal questions are recognised.** `build_frame` detects temporal
cues deterministically for every parser, so the trained models' label set
is unchanged. It then:
- maps the question to an intent whose observations cover the metric;
- masks time phrases from slot extraction ("the last five minutes" is not
  "the last one");
- never makes an action temporal.

**How they are answered.** Only from recorded points:
- **Trend:** a Mann–Kendall test (|z| > 1.96) **and** a minimum change
  between the first-third and last-third means: 5 pp for CPU, 2 pp for
  memory, 16 MiB for a process.
- **Onset:** the first sample of the current elevated run. It is "unknown"
  if the run was already elevated when recording began.
- **Recovery:** a peak followed by a decrease.
- **Growth ranking:** per-identity trends.

With fewer than 4 samples or less than 10 s of history, OSIE reports that
there is not enough history and how to record it. The answers say the
result is descriptive: consecutive samples are autocorrelated, so the test
alone would overstate significance, and the effect-size floor is the main
guard.

## Model discovery

`inference::locate_model` (`include/osie/inference/model_locator.hpp`)
searches, in order:
1. `--model`
2. `$OSIE_MODEL`
3. the user data directory
4. `<exe>/../share/osie/models`, which is also the build tree's layout

It never searches the working directory, and the binary embeds no source
path. A model the user explicitly requested that fails to load is an error.
Details are in [INSTALL.md](INSTALL.md#where-osie-looks-for-the-model).

## Knowledge graph

Typed nodes (`host, cpu_cluster, logical_cpu, process, thread, user, service,
memory, swap, filesystem, net_interface, cgroup, container, pod, k8s_node,
concept`) and edges (`HAS_THREAD, PARENT_OF, BELONGS_TO, RUNS_ON, MANAGES, USES,
CONTAINS, SCHEDULED_ON, OWNED_BY, PART_OF, MOUNTED_ON, LIMITS, AFFECTS,
MEASURED_BY`). A static schema (`schema_allows`) rejects ill-typed edges.

Each edge carries an `EdgeNature`:

* `invariant` — documented OS behaviour (e.g. *cgroup quota → throttling*, sched-bwc.rst);
* `observed` — read from a native interface (e.g. `PARENT_OF` from ppid);
* `inferred` — deterministically derived (systemd unit from cgroup path);
* `hypothesized` — probabilistic causal link with confidence < 1 (e.g. *run-queue length → scheduling latency*, p≈0.85).

Edges also carry platform sets, provenance and a documentation reference;
`check_consistency()` verifies schema, dangling ids, `PARENT_OF` acyclicity,
alive-edge/dead-node mismatches, and that every ontology edge cites a source.
Nodes and edges have validity intervals in graph *revisions*; incremental
updates upsert by `(kind, key)` and retire what disappeared.

What is **not** asserted: per-CPU → P/E-cluster membership on macOS (no public
API maps logical CPU indices to perf levels), thread → CPU placement, and
process → socket ownership (not read in V1).

## Reasoning

Thresholds (`reason::Thresholds`) are explicit heuristics, reported with each
conclusion, e.g. busy ≥ 85 % = saturated; one process ≥ 50 % of busy time =
dominant; PSI some avg10 ≥ 10 % = significant stall; throttled in ≥ 10 % of
periods. Notable rules that came from testing on real systems:

* On Linux the instantaneous `procs_running` is preferred over the 1-minute
  load average (which lags and also counts D-state tasks); a high load with a
  short run queue becomes a *correlation* ("demand was recent").
* Major page faults alone are **not** memory-pressure evidence (cold reads of
  executables cause them); they count only when corroborated by swap-ins.
* Swap in use without swap activity is reported as historical (correlation).
* Proposals never target OSIE's own process and are always labelled "not executed".
* When busy CPU time cannot be attributed to readable processes, OSIE says the
  remainder likely belongs to processes it cannot read without privileges.

## Performance notes

See `docs/BENCHMARKS.md`. Hot paths avoid per-call heap churn where it matters
(feature caches, `reserve`, contiguous node/edge vectors with 32-bit ids,
`partial_sort` for top-N). No SIMD/Accelerate/Metal is used: measured
inference is already tens of microseconds, below process-enumeration cost.
