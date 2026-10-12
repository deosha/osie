# Prior art (reviewed 2026-10-12 from each project's README and GitHub metadata)

| Project | What it solves | Mechanism | License | Reusable for OSIE? |
|---|---|---|---|---|
| [oracle/bpftune](https://github.com/oracle/bpftune) | Always-on auto-tuning of Linux sysctls (TCP buffers, neighbour tables, …) | BPF observability + per-subsystem "tuners"; explicit "what/why" logging; backs off when an admin changes a tunable | GPL-2.0 WITH Linux-syscall-note | Design principles (minimise overhead, explain every change, push-pull limits, get out of the admin's way). Code is GPL and Linux/BPF-only; not linked. |
| [sched-ext/scx](https://github.com/sched-ext/scx) | Loadable BPF CPU schedulers (kernel ≥ 6.12) | `sched_ext` BPF struct_ops; many schedulers (rustland, layered, …) | GPL-2.0 | A future *action* backend for scheduling policy on Linux; out of scope for read-only V1. |
| [eunomia-bpf/schedcp](https://github.com/eunomia-bpf/schedcp) | LLM agents select/synthesise sched_ext schedulers via an MCP server | Agentic LLM + workload profiling + scheduler generation | MIT | Shows the "LLM operates the OS" direction OSIE deliberately avoids; useful as a contrast and for its workload benchmarks. |
| [redhat-performance/tuned](https://github.com/redhat-performance/tuned) | Profile-based static/dynamic tuning daemon (D-Bus, `tuned-adm`) | Hand-written profiles; recommendation via static rules (`recommend.d`) | GPL-2.0 | Profile catalogue as domain knowledge; static recommendation = the deterministic baseline OSIE generalises. |
| [k8sgpt-ai/k8sgpt](https://github.com/k8sgpt-ai/k8sgpt) | Kubernetes cluster scanning and triage in plain English | Codified SRE analyzers, then an LLM (OpenAI/Bedrock/local) to explain | Apache-2.0 | Analyzer-first architecture validates OSIE's "deterministic evidence first" design; relevant to the Milestone 6 design. |
| [HolmesGPT/holmesgpt](https://github.com/HolmesGPT/holmesgpt) | Incident root-cause investigation across many data sources (CNCF sandbox) | Agentic LLM loop over "toolsets" (Prometheus, Kubernetes, Datadog, …) | Apache-2.0 | Toolset/abstraction ideas; depends on a large general-purpose LLM, which OSIE aims to make unnecessary. |

## What is already solved

* Rule- and profile-driven **tuning** of Linux (tuned, bpftune) and pluggable
  **scheduling** (sched_ext).
* LLM-driven **explanation** of cluster/infra state on top of deterministic
  analyzers (k8sgpt) or agentic tool use (HolmesGPT, schedcp).

## Open problems these projects leave

* Explanations depend on a large remote or local LLM (cost, latency, privacy,
  hallucination risk); none of them quantify how little model is needed.
* No explicit, typed OS-architecture model that distinguishes documented
  invariants from probabilistic causal links, with provenance per edge.
* macOS is unsupported by all six.
* Few measure whether predictive models beat simple rules on controlled,
  held-out workloads.

## What distinguishes OSIE

* Local, read-only, no LLM: the language layer is a ≤ 0.3 M-parameter model
  (or zero parameters), and *what to observe* is never chosen by a model.
* A typed knowledge graph with edge epistemics (invariant / observed /
  inferred / hypothesised) and documentation references, used in every explanation.
* Native macOS (Mach/libproc/sysctl/NSProcessInfo) and Linux
  (procfs/cgroup v2/PSI/rtnetlink) adapters behind honest, platform-specific contracts.
* Measured comparisons of rules vs. tiny models (intent and CPU bottleneck
  diagnosis) with held-out data, seeds and costs.

## Limitations of this review

Based on READMEs and repository metadata (license, activity) fetched on
2026-10-12; no code from these projects was read in depth, executed or
reused. No ML-efficiency papers were surveyed systematically.
