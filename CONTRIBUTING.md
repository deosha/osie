# Contributing to OSIE

Thanks for your interest. OSIE is a research prototype, so contributions
that add **measured evidence** are as welcome as code. Please read
`docs/ARCHITECTURE.md` and `docs/SECURITY.md` first; the project's rules
below follow from them.

## Ground rules

1. **Read-only.** Do not add code that changes system state (killing
   processes, renicing, writing sysctls or cgroup files, restarting services,
   network changes, Kubernetes mutations). Action capabilities may be
   *declared* in the registry so requests can be recognised and refused, but
   no executor may be added without a design discussion first
   (see "Future actions" in `docs/SECURITY.md`).
2. **No shell-outs.** Never call `system()`, `popen()`, `exec*()`,
   `posix_spawn` or `NSTask`, and never parse the output of `ps`, `top`,
   `vm_stat`, `sysctl`, `lsof` or similar tools. Use the documented
   underlying APIs.
3. **No privilege escalation and no private APIs.** Do not request root,
   entitlements or `task_for_pid`, and do not use undocumented Apple APIs or
   bypass OS protections. If data is not readable, report it as unavailable.
4. **Never fabricate a value.** A field the platform cannot provide is
   `std::nullopt` (or `Validity::partial` / `unavailable`), never `0`.
   Rates come from two timestamped samples; counters that go backwards yield
   no rate.
5. **Keep observation and hypothesis separate.** New reasoning must use the
   typed finding kinds (observation, hypothesis, correlation, causal
   explanation, suggested investigation, proposed optimization) and cite
   evidence. Ontology edges need an `EdgeNature` and a documentation reference.
6. **Models never decide privileges or observations.** A parser outputs only
   an intent, a confidence and spans; what to observe is fixed by
   `build_frame()`.
7. **Platform honesty.** Shared concepts go in `IOperatingSystem`;
   platform-only features go in `IMacExtensions` / `ILinuxExtensions`. Linux
   code must be tested on Linux, not only cross-compiled.

## Setup

macOS:

```bash
brew install cmake ninja googletest google-benchmark   # onnxruntime: optional, for tools/ort_bench
cmake -S . -B build -G Ninja && ninja -C build
```

Linux (or Linux tests from a Mac):

```bash
docker build -t osie-linux-dev -f docker/linux-dev.Dockerfile docker/
docker run --rm -v "$PWD":/src:ro osie-linux-dev bash -c \
  'cmake -S /src -B /tmp/b -G Ninja && ninja -C /tmp/b && /tmp/b/osie_unit_tests && /tmp/b/osie_platform_tests'
```

Python (ML work only): Python 3.11+ with `torch`, `numpy`, `scikit-learn`.
For ONNX export, create a venv:
`python3 -m venv --system-site-packages ml/.venv && ml/.venv/bin/pip install onnx onnxscript`.

## Before opening a pull request

Run all of these and make sure they pass:

```bash
ninja -C build                       # must build with no new warnings (-Wall -Wextra -Wconversion ...)
./build/osie_unit_tests
./build/osie_platform_tests          # on every platform you touched
cmake -S . -B build-asan -G Ninja -DOSIE_SANITIZE=address,undefined -DCMAKE_BUILD_TYPE=Debug && ninja -C build-asan \
  && ./build-asan/osie_unit_tests && ./build-asan/osie_platform_tests
cmake -S . -B build-tsan -G Ninja -DOSIE_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug && ninja -C build-tsan \
  && ./build-tsan/osie_unit_tests    # if you touched shared state or threading
python3 ml/evaluation/check_parity.py build/osie_eval   # if you touched tokenization or features
```

`clang-tidy` is configured in `.clang-tidy`; please run it on files you change
(`run-clang-tidy -p build <paths>`).

### Pull request checklist

- [ ] Tests added or updated: unit tests with `tests/support/mock_os.hpp`
      for logic; platform tests for anything that reads the real OS.
- [ ] Linux changes verified on Linux; macOS changes verified on macOS.
- [ ] No new runtime dependency without a measured reason (see
      `docs/BENCHMARKS.md` for how the custom inference path was justified).
- [ ] Docs updated (`docs/ARCHITECTURE.md`, `docs/MILESTONES.md`, …) if behaviour changed.
- [ ] Claims in docs are backed by a test or a reproducible measurement.
- [ ] No secrets, hostnames, personal paths or other machine-specific data in
      committed files (check `ml/experiments/results/` in particular).

## Code style

- C++20, RAII, explicit ownership, `Result<T>` for fallible operations (no
  exceptions across module boundaries), `std::optional` for unknown values.
- Match the surrounding code: naming (`snake_case` functions,
  `PascalCase` types), comment density and idioms. Comments explain *why*.
- Keep hot paths allocation-light, and benchmark before optimizing
  (`benchmarks/bench_main.cpp`).
- Objective-C++ only where a Foundation API has no C equivalent
  (`src/platform/macos/native_bridge.mm`).
- Python: standard library style; keep `ml/osie_ml/text.py` byte-identical
  in behaviour with the C++ tokenizer and feature hashing.

## ML and experiment contributions

The research protocol is described in `docs/RESEARCH.md`. To keep results
trustworthy:

- **Select and tune on `dev` only.** The synthetic `test` split and
  `ml/datasets/heldout_handwritten.tsv` are for final reporting. Never
  adjust rules, templates, thresholds or hyperparameters by looking at them.
- **Report several seeds** (the scripts use 3) with mean ± std, and score
  everything end-to-end through `tools/osie_eval` (intent *and* target).
- **Keep leakage controls intact:** template families and name lexicons stay
  disjoint across splits (`docs/DATASETS.md`).
- **Count every cost:** training time, estimated FLOPs, peak memory,
  artifact size, inference latency, and any data-authoring or teacher-model
  effort.
- **Report negative results.** "The model did not beat the rules" is a valid
  and useful outcome.
- **Real user utterances** are the most valuable data contribution. Only
  submit text you have the right to share, with personal information removed.

Experiments that generate load (such as `ml/experiments/cpu_contention/`)
must run in disposable, resource-limited containers or VMs, never against a
developer's host.

## Reporting issues

Please include: OS and version, CPU architecture, compiler, the exact command,
and the output of `osie snapshot --json` (review it first; it contains
process counts and system metadata, but no process names).

For security issues (for example, a way to make OSIE change system state or
read data it should report as unavailable), please contact the maintainer
privately instead of opening a public issue.

## License

By contributing, you agree that your contributions are licensed under the
Apache License 2.0 (see `LICENSE`).
