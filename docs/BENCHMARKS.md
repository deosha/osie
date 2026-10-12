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
