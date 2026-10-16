# Benchmarks

All numbers below were measured on 2026-10-12. Raw outputs are in
`ml/experiments/results/` (`bench_macos.json`, `ort_vs_custom.jsonl`,
`summary.json`, `ablation_names.json`, `cpu_contention.json`).

**Hardware:** Apple M4 Pro (8 P + 4 E cores), 24 GB RAM, macOS 15.7.3, Apple clang 17, `RelWithDebInfo`.
**Linux:** Ubuntu 24.04 container on the Docker Desktop VM (kernel 6.10 linuxkit, 12 vCPU), GCC 13.

The host was not idle: a desktop session (browser, IDE, video conferencing)
and the Docker VM were running. Treat absolute numbers as ±20%.

## Reproduce

```bash
cmake -S . -B build -G Ninja -DOSIE_BUILD_BENCHMARKS=ON && ninja -C build
./build/osie_bench --benchmark_min_time=0.3s
ml/.venv/bin/python ml/evaluation/export_onnx.py models/intent_gru.osiem /tmp/gru.onnx
./build/osie_ort_bench models/intent_gru.osiem /tmp/gru.onnx ml/datasets/heldout_handwritten.tsv
python3 ml/experiments/run_all.py            # intent models (~9 min)
python3 ml/experiments/ablation_names.py && python3 ml/experiments/learning_curve.py
python3 ml/experiments/cpu_contention/run_lab.py run && python3 ml/experiments/cpu_contention/run_lab.py analyze
```

## Runtime cost of OSIE itself (macOS, Google Benchmark)

| Operation | Time | Notes |
|---|---|---|
| CPU tick sample (`host_processor_info`) | 3.4 µs | 12 CPUs |
| VM statistics (`host_statistics64` + sysctls) | 2.7 µs | |
| Process snapshot (738 processes) | 1.02 ms | without paths |
| Process snapshot + executable paths | 1.94 ms | `proc_pidpath` |
| Filesystems (`getfsstat`) | 3.2 µs | |
| Network (`NET_RT_IFLIST2` + `getifaddrs`) | 251 µs | dominated by `getifaddrs` |
| Ontology load | 11 µs | 31 concepts, 36 relations |
| Graph incremental update | 3.6 ms | 861 nodes / 2,875 edges; hottest internal step |
| Graph consistency check | 31 µs | |
| Slowness diagnosis | 17 µs | |
| Tokenize | 0.27 µs | |
| Rule parser | 1.0 µs | |
| Neural parser, deployed bag (266k) | 6.5 µs | int8: 6.4 µs |
| Neural parser, BiGRU 291k | 36 µs | int8: 35 µs |
| Neural parser, CNN 276k | 42 µs | |
| Model load (`.osiem`, 1 MB) | 2.4 ms | |
| `ask()` with cached observations | 26.5 µs | parse + reasoning + render |
| `ask()` with fresh observations, 0 ms interval | 8.2 ms | two full samples |

### End-to-end CLI

| | Wall time | Peak RSS | Binary size |
|---|---|---|---|
| `osie ask "why is my mac slow"`, rules | 0.54 s | 7.9 MB | 688 KB |
| same with the hybrid model | 0.54 s | 10.6 MB | + 1.0 MB model (0.3 MB int8) |

The wall time is the 500 ms sampling window, which is needed to turn
cumulative counters into rates (`--interval` adjusts it). The remaining
~40 ms is mostly process enumeration and the graph update. OSIE's own CPU
use for one question is about 30 ms.

### Linux (container, 3 processes visible)

| Operation | Time |
|---|---|
| `/proc/stat` sample | 9.0 µs |
| `/proc/meminfo` + `/proc/vmstat` | 14 µs |
| rtnetlink link dump | 24 µs |
| `ask()`, cached | 10.9 µs |
| `ask()`, fresh, 0 ms interval | 0.23 ms (peak RSS 7.2 MB) |

## Custom inference vs ONNX Runtime 1.31

Both use the same weights, the same C++ hashing, a single thread, and are
measured over 210 hand-written utterances × 4 repetitions. ORT time includes
featurization (~2.5 µs) for parity with the custom path, which hashes inside `forward()`.

| Model | Parity (max \|Δp\|) | Argmax agreement | Custom mean / p99 | ORT mean / p99 | Load custom / ORT | RSS Δ custom / ORT |
|---|---|---|---|---|---|---|
| bag 266k | 3.6e-7 | 100% | **5.4 / 9.1 µs** | 32.2 / 52.4 µs | 3.7 / 27.9 ms | 2.9 / 16.5 MB |
| CNN 276k | 4.2e-7 | 100% | 32.4 / 60.4 µs | 33.6 / 53.6 µs | 3.3 / 18.5 ms | 2.9 / 18.7 MB |
| BiGRU 291k | 8.3e-7 | 100% | **31.1 / 56.7 µs** | 45.2 / 73.5 µs | 4.5 / 17.7 ms | 2.9 / 18.3 MB |

The specialised path is 1.0–6× faster per call, loads 4–8× faster, and uses
~6× less memory, with no external dependency. The naive scalar CNN is the
only case where ORT is competitive. That is why OSIE keeps its own
~300-line forward pass and ONNX is used only for this comparison.

## Intent-model training cost

Measured on CPU with PyTorch 2.9 (4 threads, M4 Pro) and 6,798 training
examples; mean of 3 seeds. FLOPs are analytic estimates covering
forward + backward over all epochs run. Peak RSS is for the whole training
process, which includes ~280 MB for PyTorch itself.

| Config | Params | Artifact f32 / int8 | Train time | Est. train FLOPs | Peak RSS | Inference |
|---|---|---|---|---|---|---|
| bag 20k | 17.5k | 69 / 25 KB | 5.1 s | 4 GFLOP | 290 MB | 4 µs |
| bag 300k (+knowledge, deployed) | 266k | 1,040 / 304 KB | 5.0 s | 11 GFLOP | 318 MB | 7 µs |
| CNN 300k | 276k | 1,080 / 344 KB | 9.7 s | 41 GFLOP | 319 MB | 35 µs |
| BiGRU 20k | 21k | 84 / 40 KB | 8.2 s | 19 GFLOP | 307 MB | 9 µs |
| BiGRU 110k | 113k | 441 / 169 KB | 11.4 s | 55 GFLOP | 319 MB | 19 µs |
| BiGRU 300k | 291k | 1,136 / 400 KB | 12.4 s | 116 GFLOP | 339 MB | 33 µs |
| BiGRU 840k | 837k | 3.3 / 1.0 MB | 16.6 s | 231 GFLOP | 407 MB | 59 µs |
| BiGRU 2.9M | 2.85M | 11 / 3.3 MB | 14.1 s | 318 GFLOP | 434 MB | 119 µs |
| BiGRU 9.6M | 9.61M | 37 / 10 MB | 22.8 s | 538 GFLOP | 652 MB | 224 µs |
| TF-IDF + logistic regression | 211k coefficients | 1.9 MB pickle | 0.7 s | — | — | 430 µs (Python) |

The whole pipeline costs:

| Stage | Wall time |
|---|---|
| Data generation | 0.15 s |
| `run_all.py`: 11 configs × 3 seeds, learning curve, diversity ablation, all evaluations | 9.1 min |
| Name ablation + step-equalised learning curve | ~8 min |

No GPU was used.

**Energy was not measured.** `powermetrics` requires root, and OSIE's policy is to never escalate privileges.

## History sampling overhead (`--watch`)

Measured on an Apple M4 Pro with a release build. The command was
`( sleep 60; echo :history-size ) | /usr/bin/time -l osie --watch 1`,
compared with the same command without `--watch`.

| | `--watch 1` | idle session |
|---|---|---|
| Samples in 60 s | 39 (each is a 0.5 s window plus a 1 s wait) | 0 |
| CPU time (user + sys) | 0.65 s, ≈ 17 ms per sample | ≈ 0 s |
| Average CPU | ≈ 1.1% of one core; ≈ 0.3% at `--watch 5` | — |
| History size | 114 KiB, ≈ 2.9 KiB per point; ≈ 2.1 MiB at the 720-point cap | 72 B |
| Peak RSS | 10.4 MB | 4.9 MB |

The per-sample cost is mostly process enumeration (736 processes on this
machine at the time).

## Fuzzing the model loader

Harness: `tests/fuzz/fuzz_model_loader.cpp`. Built with clang 18, libFuzzer,
ASan and UBSan, in the Linux dev container. Any accepted model must run a
forward pass with finite probabilities.

| Run | Instrumentation | Seeds | Executions | Coverage | Result |
|---|---|---|---|---|---|
| 1 | harness only | 8 shipped models | 3,470 | — | **Crash:** finite weights ≈ 3.4e38 gave NaN probabilities. Fixed by the weight bound. |
| 2 | harness only | 8 shipped models | 3,365,185 in 10 min | 44 edges | no crash; coverage too low to mean much |
| 3 | whole library (`-fsanitize=fuzzer-no-link`) | 4 tiny models, one per arch (`tests/fuzz/seeds/`) | 21,341,926 in 10 min (≈ 35k/s) | 1,116 edges | no crash; peak RSS 512 MB (libFuzzer and ASan) |

Run 2 shows why both the instrumentation and the seeds matter. Without
instrumenting the library, libFuzzer gets no feedback from the loader. With
megabyte-sized seeds, most mutations land in weight bytes. CI runs the
setup of run 3 for 5 minutes on every push.

## Sanitizers and static analysis

| Check | macOS | Linux |
|---|---|---|
| Unit tests (123) | pass | pass |
| Platform/integration tests | 11 pass | 15 pass, 1 skipped (only meaningful as non-root) |
| ASan + UBSan | pass | pass |
| TSan | pass | pass (unit tests) |
| clang-tidy (`bugprone-*`, `performance-*`, `clang-analyzer-*`) | — | run; no defects found; small fixes applied |

- **UBSan** found one real bug: misaligned 64-bit access in the macOS routing sysctl parser. It is fixed.
- **clang-tidy** reported no defects. Small items were applied (a by-value parameter, redundant optional conversions, a missing `reserve`). The remaining `unchecked-optional-access` reports are guarded by invariants the checker cannot see, e.g. `top_by_cpu()` returns only processes that have `cpu_pct`.

```bash
cmake -S . -B build-asan -DOSIE_SANITIZE=address,undefined -DCMAKE_BUILD_TYPE=Debug -G Ninja && ninja -C build-asan
cmake -S . -B build-tsan -DOSIE_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug -G Ninja && ninja -C build-tsan
```

## v0.2 latency budget (`osie_perf`)

Measured 2026-10-13 with `./build/osie_perf --iterations 2000 --watch-seconds 5
--world-model models/world_cpu.osiewm`. The builds are RelWithDebInfo:
- macOS 15 on the M4 Pro;
- Linux in a 1-container Docker session on the same machine (kernel 6.10,
  aarch64), seeing only its own processes.

The host was not idle; treat the numbers as ±20%. Raw JSON:
`ml/experiments/results/perf_v02_{macos,linux}.json`.

p50 / p99 in µs:

| Stage | macOS | Linux |
|---|---|---|
| Rule parse | 1.2 / 1.9 | 1.6 / 3.6 |
| Understanding pipeline (normalise, safety, parse, frame, validate) | 2.1 / 3.9 | 3.0 / 11.4 |
| Observe: system info | 9.5 / 11.9 | 24.5 / 50.8 |
| Observe: CPU topology | 13.0 / 18.3 | 91.4 / 345.1 |
| Observe: CPU ticks | 4.6 / 12.6 | 9.0 / 32.5 |
| Observe: memory | 2.8 / 4.5 | 8.0 / 27.5 |
| Observe: process list | 1 982 / 4 093 (≈ 720 processes) | 7.9 / 198 (3 processes) |
| Observe: filesystems | 3.4 / 29.3 | 50.6 / 235 |
| Observe: network interfaces | 266 / 345 | 42.3 / 183 |
| Observe: CPU capacity | 13.3 / 14.9 | 13.3 / 34.4 |
| Observe: PSI | n/a | 10.7 / 30.6 |
| Observe: one cgroup | n/a | 34.2 / 258 |
| Diagnose CPU | 10.0 / 13.6 | 3.2 / 17.8 |
| Diagnose memory | 13.6 / 20.3 | 3.5 / 7.4 |
| Diagnose slowness | 25.3 / 35.1 | 10.7 / 29.8 |
| Knowledge-graph instance update | 3 583 / 11 847 (≈ 720 processes) | 4.5 / 37.9 |
| World-model load | 109 / 186 | 153 / 598 |
| World-model prediction | 1.3 / 1.5 | 1.0 / 1.2 |

End to end, with `ask()` and no cache reuse:

| | macOS | Linux |
|---|---|---|
| Sampling window (configured) | 500 ms | 500 ms |
| Total, p50 | 535 ms | 507 ms |
| Computation beyond the window, p50 / max | 35 / 42 ms | 7 / 25 ms |
| Peak RSS | 12.3 MB | 10.5 MB |
| `--watch` sampling every 1 s | 1.6% of one core | 0.1% of one core |

Cgroup questions on Linux (`cgroup_status`, `container_throttle`) read each
inspected cgroup twice, a window apart, to report throttling *rates*. That
adds one sampling window (+0.5 s) to those questions only.

Against the provisional v0.2 targets:
- world-model inference p99 is 1.5 µs (target < 100 µs);
- rule parsing p99 is 3.6 µs (target < 1 ms);
- there is no mandatory daemon and no network use.

### v0.1 → v0.2 regression check (macOS, release builds)

The installed v0.1.0 binary (Homebrew) and a v0.2.0 release build, on the same
5 questions × 3 repetitions (`ml/experiments/results/overhead_v01_vs_v02_macos.json`):

| | v0.1.0 | v0.2.0 |
|---|---|---|
| Wall time per question, median | 0.52 s | 0.53 s |
| Peak RSS, median / max | 7.3 / 8.0 MB | 7.5 / 8.5 MB |

Per question, the two versions were within 10 ms of each other. One v0.2 run
took 1.1 s; it did not reproduce on re-measurement and was a cold start.

### Sanitizers and fuzzing (v0.2)

- **ASan + UBSan, TSan and `-Werror`:** 237 unit tests pass on macOS.
- **Linux (Docker):** 242 unit and platform tests pass.
- **`.osiewm` loader fuzzing:** `osie_fuzz_world_model`, Linux, clang 18,
  ASan + UBSan, 4 minutes, 9.1M executions, no findings. It also runs in CI
  for 3 minutes.
- **Utterance size:** a 1 MiB utterance cost 0.4 s, linear and dominated by
  typo correction. Utterances are now cut at 4 KiB.

## Agent (`osie-agent`, Feature 1)

Measured 2026-10-15 with the release `.deb`, run as an unprivileged user
(`osie-agent --duration S --report --measure-freshness`). The host was a
Docker Desktop Linux VM: kernel 6.10, aarch64, 12 vCPUs, on an M4 Pro.
Raw output: `ml/experiments/results/agent/`.

"Host-wide" runs use `--pid=host`, so the agent sees all 466 processes of the
VM. The first run sees only its own container (a handful of processes).

| Run | Interval / window | Duration | CPU (share of one core) | RSS | Start jitter p50 / p99 / max | Snapshot age p50 / p99 / max | Collections, failures, skipped ticks | Shutdown |
|---|---|---|---|---|---|---|---|---|
| Container view, defaults | 5 s / 0.5 s | 600 s | **0.11%** | 2.7 MB | 2.1 / 6.3 / 8.3 ms | 2.50 / 4.96 / 5.01 s | 120, 0, 0 | 1.9 ms |
| Host-wide, defaults | 5 s / 0.5 s | 300 s | **0.35%** | 3.4 MB | 1.4 / 10.4 / 10.4 ms | 2.51 / 4.96 / 5.01 s | 60, 0, 0 | 0.7 ms |
| Host-wide, fast | 1 s / 0.5 s | 300 s | 1.7% | 3.9 MB (peak 4.2) | 2.5 / 10.8 / 11.1 ms | 0.50 / 0.99 / 1.02 s | 300, 0, 0 | 0.7 ms |
| Host-wide, full history | 250 ms / 100 ms, capacity 720 | 420 s | 3.9% | **5.4 MB** (peak 5.6) | – | – | 1680, –, 0 | – |
| GitHub-hosted VM (x86-64 AMD EPYC, 4 vCPU, Linux 6.17), defaults, host-wide | 5 s / 0.5 s | 300 s | **0.25%** | 4.1 MB | 0.09 / 0.41 / 0.41 ms | 2.50 / 4.93 / 4.98 s | 60, 0, 0 | 0.1 ms |

**Against the targets:**
- RSS < 20 MB: met. The history holds 2.2 MB at its default capacity of 720
  points and 466 processes.
- < 0.5% of one core at idle: met at the default 5 s interval (0.35%
  host-wide). Faster intervals cost proportionally more; most of the cost is
  reading `/proc/<pid>/*` for every process twice per collection.

**Other numbers:**
- Snapshot age is uniform between 0 and the interval, as expected for a
  reader at a random moment.
- The newest collection took about 0.5 s, almost all of it the configured
  sampling window.
- Under systemd, `systemctl stop` returned in 142 ms
  (`agent_systemd_test.sh`); the agent itself logs stops of 0.7–3 ms.
- The CI `agent-systemd` job repeats the default-configuration measurement
  on a GitHub-hosted x86-64 VM (last row). Jitter is lower there than on
  Docker Desktop, whose VM shares a laptop CPU with a desktop session.

### Agent under load: high process counts, churn, prolonged runs

Measured 2026-10-16 (raw output in `ml/experiments/results/agent/`):
- `soak_*.txt` and `heap_macos_2000p.txt`: from
  `packaging/agent/agent_soak.sh <agent> 600 2000 50 1s`, which runs
  2,000 sleeping processes, replaces 50 short-lived ones every second, and
  sets a 1 s interval with a 120-point history;
- the in-process rows: from the `AgentScale.*` and
  `AgentLinuxLive.ThousandsOfRealProcessesWithChurn` tests.

| Run | Collections, failures, skipped | CPU (one core) | Memory | Notes |
|---|---|---|---|---|
| Linux soak, 10 min (Docker Desktop VM, 6.10, aarch64), unprivileged, installed `.deb` | 600, 0, 0 | 5.2% | RSS 4.3 MB at the end, peak 8.2 MB; median 6.9 MB (middle third) → 6.4 MB (last third) | collection 236 ms (200 ms window); jitter p99 10.6 ms; stop 0.8 ms |
| macOS soak, 10 min (arm64) | 433, 0, – | 1.0% | live heap median 1,283 kB → 1,286 kB; RSS 12–25 MB | the `heap` tool suspends the agent while it samples, so the tick counts and CPU here are not timing results |
| macOS live heap, 11 min, sampled every 60 s | – | – | live heap 1.31 MB between collections, 2.09 MB mid-collection, flat from minute 2; RSS 12–23 MB | RSS swings by about 10 MB with no growth in live memory: macOS keeps freed malloc pages resident lazily |
| Live Linux test: 1,000 child processes, 20 replaced every 25 ms (Release) | 20 in 5 s, 0 | – | history ≤ 16 points × 16 processes | 139.5 ms per collection (100 ms window); stop 1 ms |
| Mock, 20,000 processes (Release, macOS) | 40, 0 | – | history 108,616 bytes, against 105,352 bytes for 200 processes | worst collection 8.7 ms (1 ms window), against 1.4 ms for 200 processes |

**What this shows.**
- **No growth with uptime.** Memory stays flat over 10 minutes with
  thousands of processes and constant churn: RSS on Linux, live heap on
  macOS.
- **History size does not depend on the process count.**
- **On macOS RSS can exceed the 20 MB target transiently** with about
  2,500 processes, peaking at 25–27 MB, while live memory stays near 2 MB.
  This is allocator page retention, not data the agent holds. On Linux the
  same workload peaks at 8.2 MB.

## Local API (`osie-agent`, Feature 2)

Measured 2026-10-16 with `packaging/agent/api_bench.sh` (raw output in
`ml/experiments/results/api/`). The agent ran unprivileged with its default
interval (5 s, 500 ms window), with `api_rate_limit = 10000` so that the load
phases measure the server rather than the default limit of 50 requests a
second.

How each phase was run:
- **Idle phases** compare the same agent with the API off and on, for
  120 s each.
- **Latency** is per request from the client's side: connect, send,
  receive, close. Each figure is over 200 requests.
- **"Concurrent"**: 16 clients in a loop for 10 s.
- **"Burst"**: 200 connections opened at once.
- **"Shutdown"**: SIGTERM while 16 clients are active.

The Linux (aarch64) column is the installed `.deb` in a Docker Desktop VM
with `--pid=host` (kernel 6.10, 12 vCPUs, 482 processes). The macOS column is
a development build in a user session. CI repeats the run on GitHub-hosted
Linux x86-64 and arm64 VMs (`api-bench.jsonl` in the job log).

| Measurement | Target | Linux aarch64 | macOS arm64 |
|---|---|---|---|
| Idle CPU, API off → on | negligible | 0.366% → 0.355% (no difference) | 0.36% → 0.37% |
| Idle RSS, API off → on | < 10 MB more | 3.88 → 4.13 MB (+0.25 MB) | 6.95 → 7.28 MB (+0.33 MB) |
| Startup to first `ready` | – | 569 ms (the first collection: 500 ms window) | 556 ms |
| `/health/live` p50 / p99 | – | 0.09 / 0.15 ms | 0.04 / 0.18 ms |
| Cached `/system/snapshot` p50 / p99 | p99 < 20 ms | 0.08 / 0.15 ms | 0.07 / 0.24 ms |
| `/system/history` (120 points) p50 / p99 | – | 0.07 / 0.12 ms | 0.11 / 0.26 ms |
| Diagnose "what is using the most CPU?" p50 / p99 | p99 < 100 ms | 0.10 / 0.22 ms | 0.10 / 0.34 ms |
| Diagnose "why is my machine slow?" p50 / p99 | p99 < 100 ms | 0.14 / 0.20 ms | 0.19 / 0.28 ms |
| Diagnose "which process uses the most memory?" p50 / p99 | p99 < 100 ms | 0.10 / 0.12 ms | 0.11 / 0.14 ms |
| 16 concurrent clients: answered/s, p50 / p99 | – | 11,980/s; 1.0 / 3.3 ms | 11,992/s; 0.35 / 1.4 ms |
| Burst of 200 connections | 16 admitted; excess refused | 38 answered, 162 `503 busy` (16 in flight, 16 queued at most) | 62 answered, 103 `503 busy`, 35 `429` |
| Peak RSS under that load | – | 8.3 MB | 11.7 MB |
| Collections during the load | no regression | 3, 0 failures, 0 skipped; start jitter p99 1.0 ms | 3, 0 failures, 0 skipped; start jitter p99 5.1 ms |
| Shutdown with 16 active clients | < `TimeoutStopSec` (10 s) | 28 ms | 40 ms |

GitHub-hosted VMs (CI `test` jobs, 2026-10-16): 4 vCPUs, Linux 6.17, the
RelWithDebInfo build, idle phases of 60 s. These runners stayed below the
rate limit, so the concurrent figure is the server's own throughput.

| Measurement | linux-x86_64 VM | linux-arm64 VM |
|---|---|---|
| Idle CPU, API off → on | 0.259% → 0.265% | 0.164% → 0.163% |
| Idle RSS, API off → on | 5.34 → 5.68 MB (+0.34 MB) | 4.38 → 4.67 MB (+0.29 MB) |
| Startup to first `ready` | 540 ms | 541 ms |
| `/health/live` p50 / p99 | 0.05 / 0.06 ms | 0.04 / 0.08 ms |
| Cached `/system/snapshot` p50 / p99 | 0.07 / 0.13 ms | 0.06 / 0.10 ms |
| `/system/history` (120 points) p50 / p99 | 0.05 / 0.07 ms | 0.04 / 0.06 ms |
| Diagnoses (top CPU, slow, top memory, trend), p99 | 0.18, 0.22, 0.15, 0.10 ms | 0.15, 0.20, 0.16, 0.08 ms |
| 16 concurrent clients: answered/s, p50 / p99 | 11,031/s; 1.5 / 3.8 ms; no refusals | 11,364/s; 1.4 / 4.1 ms; no refusals |
| Burst of 200 connections | 42 answered, 158 `503 busy` | 52 answered, 148 `503 busy` |
| Peak RSS under load | 8.1 MB | 7.0 MB |
| Collections during the load | 3, 0 failures; jitter p99 0.07 ms | 3, 0 failures; jitter p99 0.06 ms |
| Shutdown with 16 active clients | 81 ms | 62 ms |

**Reading the numbers.**
- **Diagnosis is cheap.** The evidence is already in memory, so a diagnosis
  is parsing, reasoning and rendering over one snapshot: tenths of a
  millisecond. Diagnoses are serialised on one engine.
- **On the Docker and macOS hosts, throughput was set by the rate limit,**
  not the server: the clients sent more than 10,000 requests a second, and
  every refusal there was a 429. The 4-vCPU CI runners stayed under the limit
  at about 11,000 a second. Under the
  default `api_rate_limit = 50`, one user gets 50 a second (bursts of 100).
- **Burst refusals are immediate.** A burst beyond 16 in flight plus 16
  queued gets an immediate `503 busy` with `Retry-After`. The kernel's
  listen backlog only absorbs the burst; admission is decided after
  `accept()`.
- **Memory under pressure is bounded** by the workers and the queue. Each
  in-flight diagnosis for a redacted peer holds one copy of the snapshot.
- **The systemd stop** in the integration test, with a client stalled
  mid-request, took 146 ms. The stalled client got `503 stopping`.

### Local API: concurrency, Feature 1 baseline and OS access

Measured 2026-10-16, with the hardening changes.

**Setup.**
- The installed `.deb`, run unprivileged in a Docker Desktop VM (kernel
  6.10, aarch64, 12 vCPUs) with `--pid=host`. Release build (`-O2`, the
  release-builder image).
- 2,000 extra sleeping processes: about 2,490 visible.
- Default interval (5 s, 500 ms window), and a configuration reload every
  2 s throughout.
- One user plays every client, so `api_max_clients_per_user` was set to 16
  and `api_rate_limit` to 10,000.

**Commands.**
- `packaging/agent/api_sweep.sh /usr/bin/osie-agent osie_api_bench 2000 10`;
- `ml/experiments/results/api/compare.sh`.

**Workload, per level.** 10 s of mixed questions: CPU, memory, a process,
a trend, slowness, a container, an unsupported question and a 1.9 KB
question. Every 20th request disconnects right after sending.

**Measurements.**
- *Client* columns are from the clients: connect, request, response.
- *Server* columns come from the agent's own timings (SIGUSR1): time in
  the queue, waiting for the engine lock, and inside the engine.
- *Agent* columns are from `/proc/<pid>`.

| Clients | Answers/s | p50 / p95 / p99 (client) | Queue wait p99 | Engine-lock wait p99 | Engine p99 | Agent CPU (cores) | RSS / peak | Threads, fds | 429s |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 6,461 | 0.12 / 0.39 / 0.42 ms | 0.031 ms | 0.00 ms | 0.33 ms | 0.77 | 7.8 / 9.1 MB | 20, 7 | 0 |
| 4 | 11,397 | 0.27 / 0.76 / 0.97 ms | 0.058 ms | 0.78 ms | 0.35 ms | 1.64 | 11.7 / 11.7 MB | 20, 7 | 236 |
| 8 | 9,834 | 0.48 / 1.39 / 1.96 ms | 0.060 ms | 1.70 ms | 0.35 ms | 1.48 | 9.2 / 11.7 MB | 20, 7 | 4,067 |
| 16 | 9,775 | 0.47 / 2.26 / 3.46 ms | 0.055 ms | 2.68 ms | 0.34 ms | 1.42 | 11.2 / 11.7 MB | 20, 7 | 14,655 |
| 32 | 9,285 | 0.46 / 2.41 / 4.09 ms | 0.064 ms | 2.42 ms | 0.34 ms | 1.54 | 12.5 / 12.5 MB | 20, 7 | 42,171 |
| 100 | 9,215 | 0.42 / 1.84 / 3.50 ms | 0.085 ms | 2.06 ms | 0.35 ms | 1.60 | 12.2 / 12.5 MB | 20, 7 | 156,961 |

- **Latency and contention.** Every level stays below 4.1 ms at p99. No
  request hit its deadline, and no client was refused for lack of a worker.
  Zero `503 busy` at any level. The 429s are the rate limit (10,000/s, which
  the clients exceed from 4 upward) and, from 16 clients upward, the
  per-user cap of 16: one user plays every client, and disconnecting clients
  briefly hold extra slots.
  - Diagnoses are serialised on one engine. The engine-lock wait is the cost
    of that: at most 2.7 ms at p99, while the work inside the engine is
    0.35 ms. A finer-grained engine is not justified by these numbers.
- **Memory.** Before this change, peak RSS under load was
  **31.0 MB**: glibc gave each of the 16 worker threads an arena
  and kept their freed memory. Capping arenas at 2 (`mallopt(M_ARENA_MAX, 2)`
  in `osie-agent`, glibc only) brings the peak to **12.5 MB**, with
  the same throughput.
- **Shutdown** with 100 active clients took 52 ms.
- **Before the hardening,** a request from a diagnostics-tier peer cost
  about 13.5 ms at 5,000 processes, against 0.06–0.4 ms for most full-tier
  requests. Every redacted state was a new object, which made the engine
  rebuild its knowledge graph's instance nodes inside the lock, although no
  API answer reads them. The API's engine no longer builds them
  (`EngineConfig::track_instances`). Now every request costs 0.01–0.6 ms,
  whatever the tier: 2.7 ms at most with 20,000 processes. This is measured
  single-threaded, through `DiagnosticApi::handle`.

**Against Feature 1** (`main` at `c3851d7`): same host, about 2,000 extra
processes, 120 s idle each.

| Variant | CPU (one core) | RSS / peak | Threads | Open fds | Snapshot age p99 |
|---|---|---|---|---|---|
| feature1 | 1.12% | 6.3 / 6.3 MB | 4 | 4 | 4.95 s |
| feature2 api off | 1.20% | 7.3 / 7.8 MB | 4 | 4 | 4.94 s |
| feature2 api on | 1.15% | 7.8 / 7.8 MB | 21 | 9 | 4.95 s |

- **CPU** is unchanged within noise.
- **RSS** is about 1 MB more with the API off, and 1.5 MB more with it on.
  Most of that is the larger binary: the API code, the HTTP parser and the
  engine's evidence plumbing are linked in either way.
- **With the API on,** the extra 17 threads are the acceptor and 16 idle
  workers, and the 5 extra descriptors are the socket, the directory lock
  and the wake pipe.

**No additional OS observation per request.** `strace -f -e trace=openat`
counted the agent's opens of `/proc` files for 40 s, idle and then with 100
concurrent API clients (Feature 2, API on, ~2,000 extra processes):

| Run | Collections | `/proc` opens | Opens per collection | API answers |
|---|---|---|---|---|
| idle | 8 | 96,386 | 12,048 | 0 |
| load 100 clients | 8 | 96,428 | 12,054 | 60,532 |

Answering tens of thousands of requests added no `/proc` reads: opens per
collection are within six (process churn). Snapshot freshness was the same
in both runs (p99 about 5.0 s, the interval). In the unit tests the same
property is asserted with an instrumented OS
(`ApiTruth.ConcurrentDiagnosesNeverObserveTheOs`):
- the OS calls equal two per collection;
- the API engine's refusing backend records zero attempted observations
  during 100 concurrent diagnoses.
