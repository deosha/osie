# OSIE agent (`osie-agent`)

An **opt-in** background process that observes the machine on a fixed
schedule and keeps a bounded, in-memory history. The optional local
diagnostic API ([`API.md`](API.md)) answers questions from this runtime's
snapshots and history; it is off unless `api_enabled = true`. The `osie` CLI
does not need the agent.

**What it is not:**
- not a network service: it opens no IP socket. The optional API is a Unix
  domain socket only.
- not persistent: it writes nothing to disk. The API's socket lives in the
  runtime directory `/run/osie` and is removed at stop.
- not an actor: it cannot change system state, because the read-only policy
  and the absence of any executor are the same as for the CLI.

## Running it

**With systemd** (Linux packages install the unit but never enable it):

```bash
sudo systemctl enable --now osie-agent      # opt in
systemctl status osie-agent
journalctl -u osie-agent
sudo systemctl reload osie-agent            # re-read /etc/osie/agent.conf (SIGHUP)
sudo systemctl disable --now osie-agent     # opt out
```

**By hand** (any platform, as an ordinary user):

```bash
osie-agent                                   # defaults; Ctrl-C to stop
osie-agent --config ~/agent.conf --duration 600 --report
osie-agent --once                            # one collection; prints snapshot summary, health and capabilities
osie-agent --check-config /etc/osie/agent.conf
osie-agent --print-default-config
```

**Signals:**

| Signal | Effect |
|---|---|
| `SIGTERM`, `SIGINT` | stop; an in-flight collection is cancelled, not waited for |
| `SIGHUP` | reload the configuration; an invalid file is reported and the current one kept |
| `SIGUSR1` | write the health status (JSON) to stderr |

**Exit codes:**

| Code | Meaning |
|---|---|
| 0 | clean stop |
| 1 | runtime error |
| 2 | configuration or usage error. systemd does not restart on it (`RestartPreventExitStatus=2`). |

The agent refuses to run as root unless given `--allow-root`: it needs no
privileges. On Linux it also sets `PR_SET_NO_NEW_PRIVS`.

## Configuration

`key = value` lines, `#` comments. Parsing is strict: unknown keys,
duplicates and out-of-range values are rejected. An example is installed as
`share/osie/agent.conf.example`.

**File safety.** The configuration decides what the agent observes, so the
file, and everything that could replace it, must be controlled by root or by
the agent's own user. The loader refuses:
- a file owned by another user, or writable by group or others;
- a non-regular file (a directory, FIFO or device). A FIFO never blocks the
  loader.
- a file over 64 KiB, counted on the bytes actually read;
- a path through a directory owned by another user, or writable by group or
  others without the sticky bit (so `/tmp` is acceptable);
- a symbolic link owned by another user. Links owned by root or by the
  agent's user are followed, and their targets are checked the same way.

The path is never trusted as a whole:
- Each directory is opened relative to the previous one with `O_NOFOLLOW`
  and validated on its file descriptor.
- The file is validated with `fstat()` on the descriptor that is then read.

So renaming a file into place, or retargeting a link, during a load cannot
substitute an unchecked file. The same loader is used at start, on
`SIGHUP` and by `--check-config`.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `interval` | 5s | 250ms – 1h | time between collection starts (monotonic clock) |
| `sample_window` | 500ms | 50ms – 5s, ≤ interval / 2 | window for rates (CPU, per-process CPU) |
| `history_capacity` | 720 | 8 – 100000 | history points kept in memory |
| `history_top_k` | 16 | 1 – 64 | processes kept per point (top by CPU and by memory) |
| `cgroup_top_n` | 8 | 0 – 64 | Linux: cgroups read per collection (of the busiest processes) |
| `failure_threshold` | 3 | 1 – 100 | consecutive failures of one kind before "degraded" |
| `observe` | cpu_times, load_average, memory, swap, memory_pressure_level, process_list, pressure_stall, cgroup | ObservationKind names | what to observe |
| `log_level` | info | error, warn, info, debug | |
| `health_log_interval` | 0 (off) | 0 – 1 day | one health line in the log at this interval |

These keys are rejected with an explicit message: `listen`, `bind`, `port`,
`persist`, `persistence`, `database`, `remote`, `upload`, `endpoint`. This
version has no network listener, no persistence and no remote access.

## Design

```text
 osie-agent main thread ─ signal thread (sigwait: TERM/INT/HUP/USR1)
        │
        ▼
 AgentRuntime ── scheduler thread ──► StateSampler ─► ObservationBroker ─► SafetyPolicy + AuditLog ─► IOperatingSystem
        │              (monotonic deadlines; one collection at a time)
        ├─ snapshot  : shared_ptr<const AgentSnapshot>, swapped under a pointer-sized lock
        ├─ history   : StateHistory (bounded), copied out under a short lock
        └─ health    : per-kind status, counters, jitter ring, degraded reasons
```

**Same core as the CLI.** The agent reuses `ObservationBroker` (every
observation is authorised and audited), `StateSampler`, `SystemState` and
`StateHistory`. Identity is (PID, start time) everywhere: a reused PID is a
different process in the history.

**Scheduling.**
- Each collection starts at `start + k × interval` on the monotonic clock.
- A collection that overruns skips the ticks it missed (`skipped_ticks`)
  rather than queueing them, so there is never a backlog and never an
  overlap.
- There is one admission slot, shared by the scheduler and `collect_now()`.
  A manual collection is refused while any collection is in flight. A tick
  that finds a manual collection running is skipped and counted. There are
  no work queues.
- Start jitter (actual minus scheduled start) is kept in a 1024-entry ring.
- Reported times are wall clock.

**Consistency for readers.**
- A collection builds a complete `SystemState`, then publishes it as an
  immutable snapshot by swapping a `shared_ptr` under a lock held only for
  the swap.
- A reader copies the pointer and then reads without locks. It can never see
  a half-built state, and it never waits for a collection.
- A collection interrupted by shutdown is not published.

**Lifetime, ownership and concurrency contract**
(`include/osie/agent/runtime.hpp`):
- **Lifetime: the caller's obligation.** The runtime must outlive every call
  to its members.
  - Its destructor may begin only after every member call from other threads
    has returned, and no call may begin once it has started.
  - Destroying it concurrently with a member call on another thread is
    undefined behaviour. The runtime cannot prevent this: a thread still
    entering a member function may touch a mutex or field that the
    destructor is freeing. Internal cancellation does not change that.
  - The owner orders destruction in one of two ways:
    - call `stop()`, join every thread that calls the runtime, then destroy
      it;
    - or give each calling thread a `std::shared_ptr` for as long as it makes
      calls, so that the last reference to be dropped runs the destructor.
  - `stop()` returning does not mean other threads have left their calls: a
    cancelled `collect_now()` may still be returning. So `stop()` alone does
    not make destruction safe.
  - `osie-agent` follows the first pattern.
- **Ownership.** The runtime owns:
  - the OS backend, broker, sampler, policy, audit log and history;
  - the scheduler thread, which the destructor stops and joins. Destroying
    a running runtime is therefore safe as far as that thread is concerned.

  Snapshots own their data, so a reader may keep one after the runtime is
  destroyed. `health()` and `history()` return copies.
- **Threads.** While the runtime is alive, every public member may be called
  from any thread, concurrently with every other member, including `stop()`.
  The scheduler thread never calls `stop()` and never runs the destructor.
- **Shutdown.** `stop()`:
  - first closes admission, so neither a scheduled nor a manual collection
    can start;
  - then cancels the in-flight collection, joins the scheduler, and returns
    only when no collection is in flight.

  Afterwards the runtime can be restarted or used for `--once`.
- **State.** Once `stop()` returns the state is `stopped`. A collection that
  finishes while the runtime is stopping cannot set it back to `running`.
- **Reconfiguration** is serialised. The configuration, history bounds and
  per-kind health always come from the same call. A collection already in
  flight finishes with the configuration it started with.

**Failure handling.** Each observation kind is tracked separately:
- **Recorded failures.** A failure is a recorded reason, such as permission
  denied, counters going backwards (reset or rollover) or a read error.
  Collection of the other kinds continues.
- **Degraded state.** After `failure_threshold` consecutive failures of one
  kind the agent reports `degraded`, with reasons. It returns to `running`
  when the kind recovers.
- **Unsupported kinds are not failures.** Examples are PSI on a kernel
  without it, and cgroups on cgroup v1.
- **Processes that disappear are not failures.** A process that exits
  between listing and inspection, or before its cgroup is read, is counted
  by the sampler, not as a collector failure.

**Health** (`to_json(HealthStatus)`, also printed by `--once` and on
`SIGUSR1`) reports:
- the state;
- collection, failure, skipped-tick and refused-collection counters;
- the newest snapshot's wall time and monotonic age;
- the last collection's duration and start-jitter percentiles;
- history size and bytes;
- per kind: supported, allowed by policy, ok and failed counts, consecutive
  failures, the last error and the last success;
- degraded reasons.

`capabilities_json()` lists every capability with its platform support and
the policy decision; every `act.*` capability is denied.

**Bounded memory:**

| What | Bound |
|---|---|
| History | `history_capacity` points × ≤ 2 × `history_top_k` processes |
| Snapshots | the newest and the one before it (the API derives rates between them) |
| Cgroups per collection | `cgroup_top_n` |
| Audit ring | 256 decisions |
| Jitter ring | 1024 values |

A snapshot holds one `SystemState`, the same as one CLI question. The agent
keeps two: the newest and the previous one. Nothing grows with uptime.

These bounds hold across reloads as well. When a `SIGHUP` lowers
`history_capacity` or `history_top_k`, the history is trimmed during the
reload, not as points age out:
- the oldest points beyond the new capacity are dropped;
- each kept point is cut to its top `history_top_k` processes by CPU and by
  memory;
- the history is rebuilt, so the memory that was dropped is released.

Raising the bounds keeps existing points as they are.

## systemd hardening (`packaging/agent/osie-agent.service`)

- **Identity:**
  - `DynamicUser=yes`;
  - an empty `CapabilityBoundingSet` and `AmbientCapabilities`;
  - `NoNewPrivileges=yes`;
  - `SecureBits=noroot-locked`.
- **Filesystem:**
  - `ProtectSystem=strict` (nothing writable);
  - `ProtectHome`, `PrivateTmp`, `PrivateDevices`;
  - `ProtectKernelTunables`, `ProtectKernelModules`, `ProtectKernelLogs`,
    `ProtectControlGroups`;
  - `ProtectClock`, `ProtectHostname`;
  - `UMask=0077`.
  - `/proc` stays visible, because observing other processes is the
    agent's purpose.
- **Network:**
  - `RestrictAddressFamilies=AF_UNIX AF_NETLINK`: there are no IP sockets
    at all. AF_NETLINK is used for the read-only rtnetlink dump of
    interface counters, AF_UNIX for systemd's notification socket.
  - `IPAddressDeny=any`;
  - `PrivateIPC=yes`.
- **System calls:**
  - `SystemCallFilter=@system-service` minus the `@privileged`, `@mount`,
    `@debug`, `@reboot`, `@swap`, `@obsolete`, `@raw-io`, `@module`,
    `@cpu-emulation` and `@clock` groups;
  - `MemoryDenyWriteExecute`, `RestrictNamespaces`, `RestrictRealtime`,
    `RestrictSUIDSGID`, `LockPersonality`.
- **Resources:** `MemoryMax=64M`, `TasksMax=32` (16 API workers and the
  agent's own threads), `Nice=10`, `CPUWeight=20`, `IOWeight=20`.
- **Local API** (only used when enabled):
  - `RuntimeDirectory=osie`, mode 0750, removed at stop;
  - `Group=osie`: the packages' static group, whose members may use the API
    when `api_group = osie`. Without that group, systemd allocates a dynamic
    one with no members.
- **Lifecycle:**
  - `Type=notify`: the unit is "active" after the first collection;
  - `Restart=on-failure` after 5 s, at most 10 starts a minute;
  - `RestartPreventExitStatus=2`;
  - `TimeoutStopSec=10s`.

`systemd-analyze security osie-agent` rates the unit **1.1 ("OK")**, on
systemd 249 in Ubuntu 22.04.

The Debian and RPM maintainer scripts run `daemon-reload` on install and
stop and disable the agent on removal. On upgrade they restart it only if it
was running. They never enable or start it.

## Tests

| Test | What it covers |
|---|---|
| `AgentConfig.*` (unit) | parsing, every range, duplicates, typos, rejected network and persistence keys, unsafe file permissions, size limits (on bytes read); symlinks (followed, looping, to a directory or FIFO, planted in or pointing into a writable directory); group- or world-writable and sticky directories; files and directories owned by another user (as root); a writer swapping the file by rename and by symlink retargeting during 1.5 s of continuous loads |
| `AgentRuntime.*` (unit, mock OS) | start/stop, repeated restarts, stop during a 3 s sampling window, no overlap, skipped ticks, missing PSI, counter reset then recovery, transient permission failures, a process vanishing mid-sample, PID reuse, a quota change while running, bounded history after 400 collections, 8 concurrent readers with a live reconfiguration, read-only audit, health JSON |
| `AgentLifecycle.*`, `AgentConcurrency.*` (unit, mock OS) | `stop()` during a manual or scheduled collection that is blocked in the OS (cancelled and awaited); the two allowed destruction orders (stop, join, destroy; shared ownership with the last caller destroying); destruction from the owner during a scheduled collection; no admission while `stop()` waits; snapshots outliving the runtime; the state after `stop()` with racing manual collections; scheduler restarts, manual collections, reconfiguration and readers all at once (no overlap); racing reconfigurations |
| `AgentBounds.*`, `AgentScale.*` (unit, mock OS) | lowering capacity and top-k on reload trims existing points and releases memory; 20,000 processes (history bytes independent of the count); 2,000 collections with constant PID reuse (no identity merged); a 4 s scheduler run at a 1 ms interval with churn (bounded, no failures) |
| `AgentLinuxFixture.*`, `AgentLinuxLive.*` (Linux platform tests) | a fixture `/proc` with unreadable and corrupt entries and no PSI, run unprivileged; the live kernel; 1,000 real child processes, 20 of them replaced every 25 ms |
| `packaging/agent/agent_soak.sh` | the agent binary over a long run with thousands of processes and churn; fails if RSS grows after the history is full |
| `packaging/agent/agent_systemd_test.sh` | as a real service: installed disabled; unprivileged with no capabilities; no IP sockets; no files written; reload, valid and invalid; 5 restarts; stop time; bad config (exit 2, no restart loop); removal stops it |

On Linux (x86-64 and arm64) and macOS, CI runs:
- the unit and platform tests under AddressSanitizer with
  UndefinedBehaviorSanitizer;
- the unit tests under ThreadSanitizer;
- on Linux, the ownership test as root.

The `agent-systemd` job runs a 10-minute soak with 2,000 processes.

The systemd test runs locally in a privileged Ubuntu 22.04 container with
systemd as PID 1, and in CI on a GitHub-hosted VM (`agent-systemd` job).

## Measurements

See [`BENCHMARKS.md`](BENCHMARKS.md), "Agent".
