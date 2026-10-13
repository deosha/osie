# Compatibility and support matrix (v0.2)

"Stable" means implemented, covered by tests, and verified on the platforms
listed. "Experimental" means it works and is tested, but its accuracy is
established only in the conditions stated, and it is off by default.

## Functionality

| Area | Status | macOS | Linux | Notes |
|---|---|---|---|---|
| Observation (CPU, memory, processes, disks, network) | stable | ✓ | ✓ | native APIs only; unreadable data reported as such |
| Effective CPU capacity (host, online, affinity, cpuset, quota) | stable | ✓ (host/online only; others not applicable) | ✓ | unknown limits listed, never defaulted |
| Diagnosis (CPU, memory, slowness, per process, cgroup throttling, quota-bound) | stable | ✓ | ✓ | findings typed by epistemic status |
| Process listing, relationships, cgroup overview | stable | ✓ | ✓ (cgroups: v2 only) | |
| Temporal questions (trend, onset, recovery) | stable | ✓ | ✓ | in-session history only (`--watch`), no daemon, no disk |
| Language: rule parser + clause-level safety routing | stable | ✓ | ✓ | the default |
| Language: tiny learned parser (`--parser hybrid\|neural`) | optional | ✓ | ✓ | v0.1 model; not better than the rules on v0.2 sets |
| Refusal of state-changing requests | stable | ✓ | ✓ | no executor exists; every `act.*` capability denied |
| CPU prediction (`osie predict`) | **experimental, off by default** | ✗ (no cgroup bandwidth control) | ✓ inside the support envelope | see below |
| Kubernetes | not implemented | ✗ | ✗ | design only (`KUBERNETES.md`) |
| Application latency prediction | not supported | ✗ | ✗ | OSIE cannot observe it; never predicted |

## Platforms

| Platform | Build and tests | Packages | Verified |
|---|---|---|---|
| macOS 15, arm64 | ✓ (CI macos-14; locally on M4 Pro) | Homebrew, `.tar.gz` | unit, platform and schema tests |
| macOS x86_64 | ✓ (CI) | Homebrew, `.tar.gz` | CI smoke test |
| Linux arm64, glibc ≥ 2.35 | ✓ (CI; Docker on M4 Pro) | `.deb`, `.rpm`, `.tar.gz` | install/upgrade/uninstall (`packaging/upgrade_test.sh`) |
| Linux x86_64, glibc ≥ 2.35 | ✓ (CI) | `.deb`, `.rpm`, `.tar.gz` | CI `packaging` job |
| cgroup v1 hosts | builds and runs | | cgroup answers say v1 is unsupported; prediction refuses |
| RHEL 9 (glibc 2.34) | build from source | | not supported by prebuilt binaries |

## CPU prediction support envelope

The shipped world model (`models/world_cpu.osiewm`, OSIEWM2) declares where
it was validated; outside it OSIE answers "Prediction unavailable".

| Dimension | Envelope | Basis |
|---|---|---|
| CPU architecture | aarch64 | development data |
| Kernel | 6.10 | development data |
| cgroup | v2, `throttled_usec` summed per CPU | measured (`WORLD_MODEL.md`) |
| Capacity after the change | 0.5–3 cores | training range |
| Actions | CPU quota 0.5–3 cores; cpuset 1–4 CPUs; offered load ×0.5–2 | training range |
| Workloads | synthetic request servers (`arith`, `hash`, `short`, `burst`, `mixed`) and batch threads | training data |
| Host CPUs | 12 (an unintended consequence of the training data: W8) | feature ranges |

How the frozen model did on data collected after it was frozen, including
other kernels and x86-64, is in
[`WORLD_MODEL_RESULTS.md`](WORLD_MODEL_RESULTS.md) ("v0.2 confirmatory
results"). The envelope was not widened on the basis of those results; see
[`V0_2_RELEASE_READINESS.md`](V0_2_RELEASE_READINESS.md).

## Known prediction failure modes

- **Host CPU count (open defect W8).** Every training host had 12 CPUs, so
  the out-of-distribution check withholds every prediction on a host with a
  different CPU count, even with a supported architecture and kernel. In
  practice predictions are given only on 12-CPU aarch64 Linux 6.10 hosts.
  Data from 4-CPU x86-64 and arm64 hosts on kernel 6.17 show that the
  predictor would have transferred (see the results).
- Real services with application-level backpressure, caches or garbage
  collection are not in the training data.
- CPU pressure (PSI) predictions are experimental; their interval coverage on
  unfamiliar workloads is not established.
- The demand estimate is the *runnable* demand. A request backlog makes it
  overstate the long-run offered load, so after a quota increase the predicted
  usage holds only while the backlog drains.
- The intervals for usage and throttling are regime-conditional (demand /
  capacity after the change). The regime where demand reaches 1.3× capacity
  or more has only 17 calibration runs, so its 95% half-widths use a
  conservative fallback.
