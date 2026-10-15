# OSIE agent (`osie-agent`)

An **opt-in** background process that observes the machine on a fixed
schedule and keeps a bounded, in-memory history. It answers no questions by
itself. It is the runtime that a later local API (Feature 2) will read from.
The `osie` CLI does not need it and is unchanged.

**What it is not:**
- not a network service: it opens no IP socket;
- not persistent: it writes nothing to disk;
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
duplicates, out-of-range values, and files writable by group or others are
rejected. An example is installed as `share/osie/agent.conf.example`.

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
- `collect_now()` refuses while a collection is running. There are no work
  queues.
- Start jitter (actual minus scheduled start) is kept in a 1024-entry ring.
- Reported times are wall clock.

**Consistency for readers.**
- A collection builds a complete `SystemState`, then publishes it as an
  immutable snapshot by swapping a `shared_ptr` under a lock held only for
  the swap.
- A reader copies the pointer and then reads without locks. It can never see
  a half-built state, and it never waits for a collection.
- A collection interrupted by shutdown is not published.

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
| Cgroups per collection | `cgroup_top_n` |
| Audit ring | 256 decisions |
| Jitter ring | 1024 values |

A snapshot holds one `SystemState`, the same as one CLI question. Nothing
grows with uptime.

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
- **Resources:** `MemoryMax=64M`, `TasksMax=16`, `Nice=10`, `CPUWeight=20`,
  `IOWeight=20`.
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
| `AgentConfig.*` (unit) | parsing, every range, duplicates, typos, rejected network and persistence keys, unsafe file permissions, size limits |
| `AgentRuntime.*` (unit, mock OS) | start/stop, repeated restarts, stop during a 3 s sampling window, no overlap, skipped ticks, missing PSI, counter reset then recovery, transient permission failures, a process vanishing mid-sample, PID reuse, a quota change while running, bounded history after 400 collections, 8 concurrent readers with a live reconfiguration, read-only audit, health JSON |
| `AgentLinuxFixture.*`, `AgentLinuxLive.*` (Linux platform tests) | a fixture `/proc` with unreadable and corrupt entries and no PSI, run unprivileged; the live kernel |
| `packaging/agent/agent_systemd_test.sh` | as a real service: installed disabled; unprivileged with no capabilities; no IP sockets; no files written; reload, valid and invalid; 5 restarts; stop time; bad config (exit 2, no restart loop); removal stops it |

The systemd test runs locally in a privileged Ubuntu 22.04 container with
systemd as PID 1, and in CI on a GitHub-hosted VM (`agent-systemd` job).

## Measurements

See [`BENCHMARKS.md`](BENCHMARKS.md), "Agent".
