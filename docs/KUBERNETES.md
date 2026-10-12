# Milestone 6 — Kubernetes integration design (specification only)

**Status: designed, not implemented.** OSIE V1 recognises Kubernetes
questions (`Intent::kubernetes`) and answers that the capability is not
implemented. Nothing below exists in code yet except the graph node kinds
(`pod`, `container`, `k8s_node`) and schema edges (`POD -CONTAINS-> CONTAINER`,
`POD -SCHEDULED_ON-> NODE`, `HOST -PART_OF-> NODE`, `CONTAINER -BELONGS_TO-> CGROUP`).

## 1. Goals and non-goals

Goals: explain *why* a workload is slow, throttled, evicted, OOM-killed or
pending, using the same evidence discipline as the host engine; produce
*recommendations* (e.g. request/limit right-sizing) with explicit uncertainty.

Non-goals for the first version: mutating cluster state, replacing the
scheduler/VPA/HPA, cluster-wide autonomous tuning.

## 2. Typed integration

A separate adapter, `IKubernetes`, reads (never writes) through the API
server with the caller's kubeconfig or an in-cluster ServiceAccount:

| Typed object | Fields used |
|---|---|
| `Pod` | uid, namespace, phase, conditions, QoS class, nodeName, owner refs, container statuses (restarts, last termination reason: `OOMKilled`, `Error`) |
| `Container` | name, image, `resources.requests/limits` (cpu, memory), containerID (runtime prefix stripped) |
| `Node` | allocatable, capacity, conditions (MemoryPressure, PIDPressure, DiskPressure), taints |
| `Event` | reason (`FailedScheduling`, `Evicted`, `BackOff`), message, involved object |
| metrics (optional) | metrics.k8s.io usage if present; otherwise node-local cgroup data |

Each object becomes an `Observation<T>` with provenance `k8s:<apiVersion>/<kind>`
and `resourceVersion`, so freshness and identity (`uid`, not name) are explicit.

## 3. Resource relationship graph

```text
NODE ─CONTAINS→ HOST (same machine when OSIE runs on the node)
POD ─SCHEDULED_ON→ NODE
POD ─CONTAINS→ CONTAINER ─BELONGS_TO→ CGROUP ─LIMITS→ PROCESS
DEPLOYMENT/REPLICASET/STATEFULSET ─MANAGES→ POD        (owner references)
SERVICE ─SELECTS→ POD                                   (label selectors; inferred)
```

Edges from owner references are `observed`; selector matches are `inferred`.

## 4. Pod → container → cgroup mapping (node-local)

When OSIE runs on a node (DaemonSet, read-only `/sys/fs/cgroup` and `/proc`
mounts), it maps API objects to kernel objects:

1. Container ID from `status.containerStatuses[].containerID` (`containerd://<id>`).
2. cgroup path from the kubelet cgroup driver:
   * systemd driver: `/kubepods.slice/kubepods-<qos>.slice/kubepods-<qos>-pod<uid_with_underscores>.slice/cri-containerd-<id>.scope`
   * cgroupfs driver: `/kubepods/<qos>/pod<uid>/<id>`
   Both are generated and then **verified** by checking that the directory exists
   and that `/proc/<pid>/cgroup` of the container's processes points to it; a
   mismatch marks the edge `unverified` instead of guessing.
3. Read `cpu.max` (limit), `cpu.weight` (from requests), `cpu.stat`
   (`nr_throttled`, `throttled_usec`), `cpu.pressure`, `memory.current/max/high`,
   `memory.events` (`oom_kill`), `memory.pressure` via the existing
   `ILinuxExtensions` code path.

## 5. CPU/memory resource analysis

Deterministic diagnoses (same finding taxonomy as the host engine):

* **CPU throttling**: `Δnr_throttled/Δnr_periods` and `throttled_usec` rate
  above threshold → `causal_explanation` with the documented mechanism
  *cgroup quota → bandwidth throttling → scheduling latency* (sched-bwc.rst).
  The Milestone 5 experiment showed that the container's own `cpu.stat` and
  cgroup PSI separate throttling from contention perfectly in controlled runs.
* **Noisy neighbour / node contention**: cgroup `cpu.pressure` high while the
  container is *not* throttled → contention for node CPUs.
* **Memory**: `memory.events oom_kill` increments, `memory.current` near
  `memory.max`, `memory.pressure` → OOM risk; node `MemoryPressure` condition
  and eviction events.
* **Scheduling**: `FailedScheduling` events + allocatable vs. sum of requests.

Recommendation candidates (always `proposed_optimization`, never applied):
request ≈ observed p90 usage over a window, limit ≥ p99 with headroom, or
removing CPU limits for latency-sensitive Burstable pods when node capacity
allows — each with the evidence window, sample count and uncertainty.

## 6. Safe recommendation architecture

```text
observations → diagnosis → candidate recommendation → simulation/what-if (documented mechanisms only)
             → availability guardrails → human review → (future) apply via GitOps PR, never direct patch
```

Guardrails a recommendation must pass: PodDisruptionBudget respected, no
change to replicas below `minAvailable`, memory limits never below observed
max + margin, CPU requests never below observed p90, change magnitude capped
per step, rollback manifest generated. Predictive models may rank
candidates; they cannot bypass guardrails.

## 7. Admission, RBAC and authorization

* Read-only ClusterRole: `get/list/watch` on pods, nodes, events,
  replicasets, deployments, statefulsets, daemonsets; `get` on
  `metrics.k8s.io` — no `create/update/patch/delete`, no `secrets`.
* Namespace scoping by default; cluster scope opt-in.
* Node agent: read-only hostPath mounts of `/sys/fs/cgroup` and `/proc`,
  `readOnlyRootFilesystem`, `runAsNonRoot` where the cgroup files permit,
  no `privileged`, no host network.
* Future write path: changes proposed as pull requests against the GitOps
  repo (or as `kubectl diff` output); an admission webhook is out of scope
  and would never be required for read-only analysis.

## 8. Testing plan

kind/k3d clusters in CI with fixture workloads (CPU-limited pod hitting its
quota, memory-limited pod OOM-killed, unschedulable pod), asserting the
diagnosis, the cgroup mapping verification, and that the RBAC role cannot
mutate anything (`kubectl auth can-i --as`).
