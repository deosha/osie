# Evidence provenance and history quality: overhead (v0.3 Feature 4)

Raw results are in `raw/`. "base" is `main` at `dca2f48` plus the shared
contract document (`a54ff5f`); "new" is the Feature 4 branch. Both were built
from the same compiler and flags in the same container and run alternately
(ABAB) under the same conditions.

## Environment

Docker (`osie-linux-dev`, Ubuntu 24.04, g++ 13.3, `-O2 -g`), Linux
6.10.14-linuxkit, 12 vCPUs, on a laptop. Another project's container used
about one core intermittently during the runs; the live-load numbers below
are affected by it (see their spread in `raw/`), the in-process ones much
less. These are measurements on one machine, not targets.

## Method

1. **In-process** (`bench_evidence.cpp`, `raw/inprocess_*_r{1,2}.jsonl`):
   a mock Linux machine with 2,000 processes feeds a real `AgentRuntime`
   (default configuration: `history_capacity` 720, `history_top_k` 16;
   `sample_window` 20 ms so the history fills quickly); `DiagnosticApi`
   answers in-process. 300 diagnoses per question for eight questions
   (2,400 in all): engine time (`HttpResponse::engine_ms`), handle time and
   response bytes. Then the history is filled to 720 points, and its
   `approx_bytes`, the process RSS, temporal questions and
   `/v1/system/history` (720 points) are measured. The same source builds
   against both trees:

       g++ -std=c++20 -O2 -I<tree>/include -I<tree>/src -I<tree>/tests \
           -I<tree>/third_party/picohttpparser bench_evidence.cpp \
           <build>/libosie_core.a -lpthread -o bench_evidence
       bench_evidence 2000 300

2. **No extra OS reads** (`raw/proc_opens_controlled.jsonl`): the agent
   (interval 3 s, sample window 200 ms, API enabled, 40 s, unprivileged, 2,006
   processes) under `strace -f -e trace=openat`, two rounds per build. Opens
   of `/proc/<pid>/...` are normalised by opens of `/proc/<pid>/stat` (one
   per process per sample), and host-level paths are counted.
   `raw/proc_opens.jsonl` is an earlier, uncontrolled run with API traffic
   and only two collections per build under strace; its totals differ
   because the process count was not pinned. It is kept for completeness but
   not used.
3. **Snapshot freshness** (`raw/freshness_*.json`): `osie-agent --duration 60
   --report --measure-freshness` at the default configuration (5 s
   interval), API off.
4. **Idle agent and API load**: `packaging/agent/api_bench.sh` with 60 s idle
   phases (`raw/api_bench_*.jsonl`, one run each); threads and descriptors of
   an idle agent with the API after 20 s (`raw/idle_threads_fds.jsonl`); and
   `osie_api_bench --requests 300 --clients 16 --seconds 10` against a fresh
   agent per run, 5 interleaved rounds per build with 2,000 sleeping
   processes (`raw/api_load_abab5.jsonl`; a noisier 3-round run is in
   `raw/api_load_abab.jsonl`).

The `RefusingOs` refusal count is reported by the in-process benchmark and
asserted to be 0 by every `ApiTruth` scenario.

## Results

### Diagnosis at 2,000 processes (in-process, 2,400 diagnoses per run)

| | base r1 | base r2 | new r1 | new r2 |
|---|---|---|---|---|
| engine p50 (ms) | 0.0253 | 0.0227 | 0.0221 | 0.0237 |
| engine p99 (ms) | 0.0926 | 0.0760 | 0.0747 | 0.0711 |

Engine time is unchanged: provenance is computed outside the engine. p99 is
well within the 1.5× limit.

Response size and handle time (engine plus serialisation), per question,
means of the two runs:

| Question | bytes base → new | handle p50 ms base → new |
|---|---|---|
| what is using the most CPU? | 11,698 → 14,994 (+28%) | 0.062 → 0.074 |
| why is my CPU usage high? | 16,970 → 20,276 (+19%) | 0.068 → 0.081 |
| why is my machine slow? | 21,174 → 25,867 (+22%) | 0.095 → 0.108 |
| which process uses the most memory? | 4,041 → 5,444 (+35%) | 0.031 → 0.034 |
| is memory pressure high? | 5,925 → 7,330 (+24%) | 0.032 → 0.036 |
| why is nginx using so much CPU? | 3,347 → 4,198 (+25%) | 0.086 → 0.084 |
| tell me about pid 101 | 882 → 902 (+2%) | 0.013 → 0.012 |
| is a container being CPU-throttled? | 17,676 → 21,000 (+19%) | 0.090 → 0.101 |

### History memory at the default configuration (720 points, top_k 16, 2,000 processes)

| | base | new |
|---|---|---|
| `approx_bytes` | 2,465,384 | 2,471,144 (+5,760 B, +0.23%: 8 bytes per point) |
| RSS growth while filling (MB, r1 / r2) | 2.49 / 2.38 | 2.76 / 2.64 |

The live agent reports the same difference: `history_bytes` 38,720 → 38,816
at 12 points (8 bytes per point). RSS growth also includes allocator state;
the history itself grew by 0.23%, under the 25% limit.

### Historical queries (720 points)

| | base p50 / p99 ms (r1; r2) | new p50 / p99 ms (r1; r2) |
|---|---|---|
| which process is growing in memory? | 3.64 / 9.95; 3.49 / 3.90 | 3.69 / 4.27; 3.59 / 4.78 |
| has cpu usage increased in the last 10 minutes? | 0.18 / 0.28; 0.18 / 0.21 | 0.19 / 0.25; 0.18 / 0.20 |
| is memory pressure increasing? | 0.18 / 0.23; 0.18 / 0.20 | 0.18 / 0.19; 0.18 / 0.20 |
| is pid 100 using more memory over time? | 0.19 / 0.20; 0.19 / 0.22 | 0.19 / 0.21; 0.19 / 0.22 |
| `/v1/system/history` (720 points, handle) | 0.74 / 0.91; 0.73 / 0.84 | 0.79 / 0.97; 0.77 / 0.86 |

`/v1/system/history` grows from about 141.8 KB to 155.9 KB (snapshot
sequence per point, coverage). The growth ranking ("which process is
growing") is O(identities × points × top_k), unchanged from base. Coverage
is one pass over the points.

### No extra OS reads (strace, 2,006 processes)

| | base r1 | base r2 | new r1 | new r2 |
|---|---|---|---|---|
| opens per `/proc/<pid>/stat` | 3.0003 | 3.0003 | 3.0002 | 3.0003 |
| per-PID files | stat, status, cgroup | same | same | same |
| host paths per attempt | identical | identical | identical | identical |

The set of files and opens per process are identical. Host-level paths
(`/proc/stat`, `/proc/meminfo`, `/proc/vmstat`, `/proc/loadavg`,
`/proc/pressure/*`, ...) have equal counts for equal numbers of collection
attempts. The API answered every question without an OS call:
`refused_calls` 0 in all four in-process runs.

### Snapshot freshness (default configuration, 60 s)

| | n | p50 ms | p99 ms | max ms |
|---|---|---|---|---|
| base | 589 | 2,474 | 4,964 | 5,013 |
| new | 590 | 2,461 | 4,947 | 4,976 |

### Idle agent and API load

| | base | new |
|---|---|---|
| idle, no API: CPU share / RSS MB | 0.0156 / 11.06 | 0.0174 / 11.25 |
| idle, API: CPU share / RSS MB | 0.0177 / 11.53 | 0.0177 / 10.24 |
| idle with API after 20 s: threads / fds | 20 / 7 | 20 / 7 |
| load, 5 rounds (medians): answered per s, 16 clients | 1,858 | 1,873 |
| diagnose_slow sequential p50 / p99 ms (medians) | 1.11 / 4.99 | 1.23 / 4.88 |
| diagnose_top_cpu sequential p50 / p99 ms (medians) | 1.02 / 5.44 | 0.81 / 3.00 |

One single run of each (`raw/api_bench_*.jsonl`) showed 9,967 vs 7,766
answered per second; the interleaved five-round medians above do not show
a difference. Live-load figures vary by several times between rounds of
the same build in this environment, so only differences much larger than
that spread would be meaningful.
