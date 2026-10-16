# OSIE local diagnostic API (`osie-agent`, v0.3 Feature 2)

The agent can answer questions from local programs over **HTTP/JSON on a
Unix domain socket**. It is **off by default**. When enabled, it answers
with the same intelligence as the `osie` CLI, but from the persistent agent's
evidence: the published snapshot and bounded history (see [`AGENT.md`](AGENT.md)).
It never starts a second observer, scheduler or history store.

What it is not:
- not a network service: it never opens an IP socket;
- not remote: there is no fleet connectivity or gateway;
- not an actor: it cannot change system state.

```text
local client ─ HTTP/1.1 over AF_UNIX ─► acceptor ─► worker pool ─► DiagnosticApi
                 (kernel peer identity)  (access tier,   (deadline per   │
                                          rate limit,     request)      ├─ request validation
                                          bounded queue)                ├─ Engine::ask(question, session, AgentEvidence)
                                                                        │     safety router, frame validation, reasoning:
                                                                        │     the CLI's pipeline
                                                                        └─ AgentRuntime: snapshots + bounded history
                                                                              (RefusingOs: on-demand observation impossible)
```

## Enabling it

In `/etc/osie/agent.conf`:

```ini
api_enabled = true
api_group = osie        # optional: members of "osie" may use the diagnostic endpoints
```

Then restart; `api_*` settings are read at start only, not on reload:

```bash
sudo usermod -aG osie alice           # grant a user access (log in again afterwards)
sudo systemctl restart osie-agent
```

```bash
curl --unix-socket /run/osie/agent.sock http://localhost/v1/health/live
curl --unix-socket /run/osie/agent.sock \
  -H "Content-Type: application/json" \
  -d '{"question":"Why is my CPU usage high?"}' \
  http://localhost/v1/diagnose
```

Without `api_group`, the socket is mode 0600 and only root can use it.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `api_enabled` | `false` | `true`, `false` | serve the API |
| `api_socket` | `/run/osie/agent.sock` | absolute path, ≤ 100 characters | socket path; see "Socket" below |
| `api_group` | (none) | group name | members get the diagnostic tier; socket 0660 |
| `api_max_clients` | 16 | 1 – 64 | requests handled at once; as many more may wait |
| `api_max_clients_per_user` | 4 | 1 – `api_max_clients` | requests one user (uid) may have waiting or in progress |
| `api_max_request_bytes` | 16384 | 1 KiB – 1 MiB | request line, headers and body together |
| `api_request_timeout` | 5s | 100ms – 60s | to receive, answer and send one request |
| `api_max_sessions` | 128 | 1 – 4096 | conversation sessions in memory |
| `api_session_ttl` | 10m | 10s – 1 day | an idle session is forgotten after this |
| `api_max_evidence_age` | 0 (automatic) | 0 – 1h | evidence older than this is never used; automatic = 2 × `interval` + `sample_window` + 2 s |
| `api_rate_limit` | 50 | 1 – 10000 | requests per second per client user; bursts up to twice this |

## Endpoints

All responses are `application/json` with `"schema_version": "1.0"`, and
carry `Connection: close` and `Cache-Control: no-store`. Requests may carry
`X-Request-Id` (1–64 characters of `[A-Za-z0-9._:-]`), which is echoed as
`request_id`. The schemas are in `docs/schemas/api_*.schema.json`.

| Endpoint | Access tier | Success | Schema |
|---|---|---|---|
| `GET /v1/health/live` | health | 200 alive; 503 if the collector is not running | `api_live` |
| `GET /v1/health/ready` | health | 200 `ready` or `degraded`; 503 `starting`, `unavailable` or `stopping` (with `Retry-After`) | `api_ready` |
| `GET /v1/capabilities` | diagnostics | 200 | `api_capabilities` |
| `GET /v1/system/snapshot` | diagnostics | 200; 503 `insufficient_evidence` before the first collection | `api_snapshot` |
| `GET /v1/system/history` | diagnostics | 200 | `api_history` |
| `POST /v1/diagnose` | diagnostics | 200 (see statuses below) | `api_diagnose` |

### Readiness

| Status | HTTP | When |
|---|---|---|
| `starting` | 503 | no collection has completed yet |
| `ready` | 200 | the newest evidence is fresh, and no collector is failing |
| `degraded` | 200 | an optional collector (PSI, cgroups, ...) has failed `failure_threshold` times in a row |
| `unavailable` | 503 | a mandatory collector (`cpu_times`, `memory`, `process_list`) is failing, the newest evidence is older than `api_max_evidence_age`, or the collector stopped |
| `stopping` | 503 | the agent is shutting down |

### Capabilities

Lists:
- every observation kind: supported here, collected by the agent's
  configuration, present in the latest snapshot;
- the diagnosis types answered, and what is never offered (actions, numeric
  predictions, on-demand observation);
- the limits in force;
- the capability policy: every `act.*` capability is `allowed: false`.

It does not include the socket path, the group, the hostname or any other
host-identifying metadata.

### Snapshot

The newest published snapshot:
- its sequence number, collection time (UTC, ISO 8601) and age;
- CPU, load, memory and PSI;
- process counts, and the top 10 processes by CPU and by memory;
- the cgroups read;
- which observation kinds are present, and why the others are missing (an
  error code only: descriptions can contain file paths).

Each process is identified by `pid` and `start_time_us`, and carries `name`,
`uid`, `cpu_pct`, `memory_bytes`, `threads`, and `path` where allowed (see
Redaction). Environment variables and command lines are never collected.

### History

`GET /v1/system/history?window_seconds=&limit=&metric=`:

| Parameter | Default | Range |
|---|---|---|
| `window_seconds` | 3600 | 1 – 86400 |
| `limit` | 120 | 1 – 1000: the newest points are returned |
| `metric` | `all` | `all`, `cpu_busy_pct`, `mem_available_bytes`, `mem_total_bytes`, `memory_pressure`, `psi_cpu_some10`, `psi_mem_some10` |

Rules:
- Unknown, duplicate, out-of-range or percent-encoded parameters are
  refused (400).
- Points are only those the agent recorded. A missing value is `null`.
- Intervals longer than 2 × `interval` + `sample_window` between consecutive
  points are listed in `gaps`. Nothing is interpolated.
- `truncated` says whether `limit` cut the window.

### Diagnose

```json
{
  "request_id": "demo-001",
  "question": "Why is nginx using so much CPU?",
  "session_id": "local-session-01",
  "max_evidence_age_ms": 10000,
  "include_evidence": true
}
```

**Fields.**
- `question` is required: 1 to 2048 bytes of UTF-8, with no control
  characters.
- All other fields are optional.
- Unknown fields, wrong types, duplicate keys, nested values and fractional
  numbers are refused (400).
- `max_evidence_age_ms` (1 to 3600000) can only tighten the server's limit,
  never relax it.
- `include_evidence: false` omits `evidence`, `processes`, `trends` and
  `diagnosis`.

**What the server does.** It answers through the CLI's pipeline:
1. the question goes through the safety router, frame validation and
   session context;
2. it is answered by the deterministic reasoner, from the agent's newest
   snapshot and bounded history;
3. no operating-system call is made for the answer. The API's engine uses an
   OS backend that refuses every observation.

**Response.** Only the requester sees it (`api_diagnose` schema):
- **`status`:**
  - `completed`;
  - `partial`: some needed observation was missing or not collected;
  - `needs_clarification`: for example an ambiguous name or "it" with
    nothing to refer to;
  - `refused`: an action request.
- **`answer`:**
  - `summary`;
  - `assessment`, a machine code such as `cpu_dominated_by_process`,
    `action_refused` or `process_not_running`;
  - `certainty`: the reasoner's categorical heuristic, or `null`;
  - `confidence`: always `null`. No calibrated confidence exists for
    diagnoses, and none is invented.
  - `text`: the CLI's answer text.
- **`interpretation`:** intent, safety mode, target, temporal scope.
- **`observation`:** the snapshot used, its collection time, its age and the
  limit applied.
- **`evidence`:** each item comes from the diagnosis's evidence. `value` is a
  value of the snapshot the answer used; a test checks this. `observed_at` is
  that snapshot's collection time.
- **`processes`:** the processes the answer refers to, with
  (pid, start time).
- **`trends`:** for temporal questions.
- **`diagnosis`:** the full structured diagnosis, as `osie ask --json` emits
  it.
- **`limitations`:** each `{code, detail}`. The codes are
  `observation_not_collected`, `unsupported_on_platform`,
  `on_demand_observation_unavailable`, `cgroup_rates`, `permission_limited`,
  `redacted`, `no_event_history`, `prediction_not_available`,
  `missing_evidence`, `inconsistent_evidence` and `insufficient_history`.
- **`action_executed`:** always `false`.

**Outcomes that are not answers** use the error envelope:

| Situation | HTTP | `code` | Retryable |
|---|---|---|---|
| no snapshot yet | 503 | `insufficient_evidence` | yes (`Retry-After`) |
| the newest snapshot is older than allowed | 503 | `stale_evidence` | yes (`Retry-After`) |
| none of the observations the question needs is collected by the agent (e.g. disks, when `filesystems` is not in `observe`) | 422 | `insufficient_evidence` (with `limitations` naming them) | no |
| outside OSIE's scope, Kubernetes, or an uninterpretable request | 422 | `unsupported` | no |

**Truthfulness rules** (each covered by `ApiTruth.*`, against a ground
truth built from the scenario's physical parameters):
- **Contradictory observations.** Some combinations cannot all be true: a
  process using more CPU than the host has, processes together much busier
  than the host, or more memory available than exists. The reasoner takes
  its inputs at face value, so the API reports such a contradiction
  (`inconsistent_evidence`) and the status becomes `partial`.
- **Short history.** A trend question whose history is too short for any
  trend is `partial`, with `insufficient_history`. It is not answered as
  "no change".
- **A target that is not running.** A PID with no process, or an earlier
  subject that exited or whose PID was reused, is a definite answer:
  `completed`, assessment `process_not_running`, with `no_event_history`.
  The process that now holds a reused PID is never diagnosed as "it".
- **Cgroup counters.** If a cgroup's counters reset (it was recreated), or
  its quota changed between the two collections, no rate is computed. The
  `cgroup_rates` limitation says which cgroup, and why.
- **Empty cgroups.** No cgroups in a snapshot is not reported as a
  collection failure unless reading them failed.

**Evidence is never synthesised.** Questions that need no observations (for
example "what is memory compression?") are answered even when the snapshot
is stale.
- **Cgroup throttling rates** are derived from the agent's last two
  collections, on the interval between them, with no extra read of
  `cpu.stat`.
- **Capacity questions** need an on-demand read of affinity, cpusets and
  quotas, which is not in the snapshot. The answer says so
  (`on_demand_observation_unavailable`), and the status is `partial`.
- **Numeric predictions** (`osie predict`) are not offered through the API.

## Errors

Every error looks like this:

```json
{"schema_version":"1.0","request_id":"r1",
 "error":{"code":"invalid_request","reason":"unknown_field","message":"Unknown field. ...","retryable":false},
 "action_executed":false}
```

`code` is one of nine stable categories; `reason` is a finer machine string.
Messages never contain stack traces or file paths. The message for an
uninterpretable question may restate a term of the requester's own question
back to them.

| `code` | HTTP | Examples (`reason`) |
|---|---|---|
| `invalid_request` | 400 | `malformed_http`, `bad_path`, `bad_query`, `invalid_json`, `unknown_field`, `question_empty`, `duplicate_content_length`, `missing_host`, `trailing_data` |
| | 405 | `method_not_allowed` (with `Allow`) |
| | 408 | `request_timeout` |
| | 411 | `length_required` |
| | 413 | `body_too_large`, `request_too_large`, `question_too_long` |
| | 414 | `target_too_long` |
| | 415 | `content_type_required`, `unsupported_media_type` |
| | 417 | `expectation` |
| | 431 | `headers_too_large`, `too_many_headers` |
| | 505 | `http_version` |
| `unauthorized` | 401 | `unknown_peer`: the kernel reported no credentials |
| `forbidden` | 403 | `diagnostics_not_permitted`: a health-tier peer asked for diagnostics |
| `unsupported` | 404 | `unknown_endpoint` |
| | 422 | `outside_scope`, `kubernetes_not_implemented`, `uninterpretable` |
| | 501 | `transfer_encoding` |
| `insufficient_evidence` | 422 / 503 | `observations_not_collected` / `no_snapshot` |
| `stale_evidence` | 503 | `stale_snapshot` |
| `rate_limited` | 429 | `rate_limited`: over the per-user request rate; `too_many_connections`: the user already has `api_max_clients_per_user` requests in progress (both with `Retry-After`) |
| `busy` | 503 | `busy`: all slots in use; `deadline_exceeded`: the request's deadline passed while the engine answered others; `stopping` |
| `internal_error` | 500 | `internal_error` |

## Transport

- HTTP/1.0 and 1.1. One request per connection; the server closes it after
  the response. There is no keep-alive and no pipelining: data after the body
  is refused.
- Bodies are framed by `Content-Length` only. `Transfer-Encoding` (chunked)
  is refused. `Expect: 100-continue` is answered, which curl sends for larger
  bodies.
- The request line and headers are tokenised by
  [picohttpparser](../third_party/picohttpparser/README.osie.md), vendored at
  a pinned commit. Policy on top:
  - at most 32 headers and 8 KiB of them;
  - no folded headers, and no bytes outside printable ASCII;
  - one `Content-Length`, `Content-Type`, `Host`, `Expect` and
    `X-Request-Id` each;
  - `Host` is required for HTTP/1.1;
  - the target is a literal path of `[A-Za-z0-9/._-]`, without `..`, `//`
    or percent-encoding.
- Request bodies are parsed by a strict flat-object JSON parser
  (`src/api/json_in.cpp`).

## Security

**Socket.**
- **Directory.** The directory of `api_socket` is opened by the same
  descriptor-based walk as the configuration file. It must be owned by the
  agent's user (or root) and writable by nobody else, sticky or not. Every
  directory above it must be trusted too.
  - Under systemd it is `RuntimeDirectory=osie`: created at start, removed
    at stop, mode 0750, group `osie`.
  - A `flock` on the directory keeps a second agent out.
- **Existing entry.** One at the socket path is removed only if it is a
  socket owned by the agent's user that nothing answers on (a stale socket).
  A symlink, file or live socket is never replaced or followed; the agent
  refuses to start.
- **Permissions.**
  - Mode 0600, or 0660 with group `api_group`.
  - They are set before `listen()`, so there is no window in which the
    socket accepts connections with looser ones.
- **On stop**, the socket is removed only if it is still the inode the
  agent created.

**Who may ask what.** The access tier is decided from the peer's credentials,
as the kernel reports them when it connects:
- Linux: `SO_PEERCRED`, plus `SO_PEERGROUPS` for supplementary groups;
- macOS: `LOCAL_PEERCRED`.

The tier is decided before the request is read. The question's wording
cannot change it, and the tier is tested separately from the safety router.

| Tier | Who | May use |
|---|---|---|
| health | any peer that can open the socket | liveness, readiness |
| diagnostics | members of `api_group` | everything; other users' executable paths are withheld |
| full | root, the agent's own user | everything |

Two layers protect the diagnostic endpoints:
1. **Socket permissions.** Non-members cannot connect at all.
2. **Peer credentials.** If the socket's permissions were loosened by mistake
   (say 0666), anyone could connect, but would still get only the health
   tier.

A Linux test (`ApiLinuxPeers`, as root) checks both layers, with real peers
of other users.

**Redaction.** For the diagnostics tier, the state the answer is built from
is redacted before reasoning, so the answer text cannot leak what was
withheld:
- **Executable paths** of other users' processes are withheld.
  `/proc/<pid>/exe` is unreadable to them too, so this matches what the
  kernel already shows each user.
- **Under `hidepid`** (detected from `/proc`'s mount options), other users'
  processes are removed entirely.
- **Host totals** stay: every user can read them.
- **Process environments and command lines** are never collected.

**Sessions.**
- **Keys.** A session is keyed by the peer's kernel uid and its
  `session_id`. A client that guesses another user's session id gets a new,
  empty session of its own, so "it", "that process" and "the second one"
  never cross users.
  - Processes of the same uid are one principal (they can read each
    other's memory anyway), and may share a session by id.
  - Ids the server generates are 128-bit random values.
- **Limits.**
  - At most `api_max_sessions` sessions in total, and a quarter of them per
    uid, so one user cannot evict the others.
  - Idle sessions expire after `api_session_ttl`; the least recently used
    is evicted first.

**Bounds.**
- `api_max_clients` workers, and a queue of the same length. Connections
  beyond that get an immediate 503 `busy` instead of waiting.
- At most `api_max_clients_per_user` of those per uid. One user, however
  many connections it opens or leaves stalled, cannot hold every worker;
  others are still served (`ApiLinuxPeers.AdmissionIsFairAcrossUsers`).
- Diagnoses share one engine, whose lock is taken with the request's
  deadline. A request still waiting at its deadline is refused
  (`deadline_exceeded`) instead of waiting longer. A diagnosis takes
  0.03–0.35 ms at p99, so in practice the lock wait stays below 3 ms even
  with 100 clients (BENCHMARKS.md).
- A per-uid token bucket (429), whose map is bounded to 4096 uids.
- Each request's deadline (`api_request_timeout`) covers queueing, reading
  and answering, so a slow client cannot hold a worker for longer than that.
- Refused connections are answered and drained for at most 500 ms; at most
  256 are kept at once.
- Refusals are logged at most 10 times a second.
- Questions are never logged. With `log_level = debug`, one access line per
  request gives uid, method, path, status and duration.

**Read-only.** The API registers no capability and holds no executor. The
action policy is unchanged, and action requests are refused
(`status: refused`, `action_executed: false`) as in the CLI.

**Shutdown** (`SIGTERM`):
- the listener is closed first;
- queued connections get 503 `stopping`, and so do clients still sending a
  request;
- requests already being answered complete;
- all threads are joined before the runtime stops.

Every handler holds a reference to the runtime, as `AgentRuntime`'s lifetime
contract requires.

## Compatibility rules

- The path prefix (`/v1`) and `schema_version` (`"1.0"`) version the API.
- Within 1.x, fields and enum values may be added, and optional fields may
  become present more often. Clients must ignore what they do not know.
  Error `code` values are stable; `reason` values may be added.
- Removing or renaming a field, changing its type or meaning, or making the
  server stricter about a request it used to accept requires `/v2` and
  schema 2.0.

## Platform support

**Linux is the supported service target.** It has systemd integration,
`SO_PEERCRED` and `SO_PEERGROUPS`, `hidepid` detection, and the root-only
cross-user test.

**macOS: experimental, owner-only.**
- The same server runs, for development.
- Peer credentials come from `LOCAL_PEERCRED`, and the unit and socket
  tests, including kernel peer credentials, run on macOS.
- macOS has no service unit, no runtime directory and no cross-user test, so
  its isolation between users is not evaluated. The API is therefore
  restricted there:
  - `api_group` is refused, so the socket is 0600 and only root and the
    agent's own user can use it;
  - startup logs a warning;
  - `/v1/capabilities` reports `"platform_support": "experimental"`.

## Known limitations

These come from the intent parser shared with the CLI. The CLI answers the
same way, and they are left for a parser change with its own evaluation:
- **Unrecognised past periods.** "What was the CPU usage last week?" is
  answered with the current state: the period is not recognised. The
  response's `interpretation` has no `temporal` object, which shows that the
  question was read as being about now.
- **Some past events read as current problems.** "Why did nginx crash an
  hour ago?" is read as a current slowness question, while "why was nginx
  killed?" is recognised as a past event (`no_event_history`). OSIE keeps no
  event history either way.
- **Some growth questions routed to process information.** "Is nginx's
  memory increasing?" (with the possessive) and "which process's memory
  grew?" go to `process_info` rather than a trend. "Is nginx memory
  increasing?" and "which process is growing in memory?" are trend
  questions.
- **Bare ordinals.** "The second one" on its own is not understood, even
  after a list. "Why is the second one using CPU?" is.

## Tests

| Test | What it covers |
|---|---|
| `ApiHttp.*`, `ApiJson.*` | framing and header policy, paths and queries, NUL bytes, oversize, strict JSON (UTF-8, surrogates, controls, duplicates, nesting, numbers) |
| `ApiEndpoints.*`, `ApiDiagnose.*` | every endpoint; readiness states; CPU, memory, process-targeted and throttling diagnoses; evidence values equal to snapshot values; no OS call for covered questions; unknown, unsupported and uncollected questions; stale and missing snapshots; process exit and PID reuse; actions and prompt injection; secrets; request validation |
| `ApiAuthorization.*`, `ApiSessions.*` | tiers by identity whatever the question; redaction and hidepid; sessions isolated across users and ids, bounded and expiring |
| `ApiConcurrency.*` | 100 concurrent diagnoses with live sampling and reconfiguration |
| `ApiServer.*` (real socket) | disabled by default; every endpoint over the socket; 0600 and 0660 modes; unsafe directories, symlink and file at the path, stale and live sockets, one agent per directory, replacement at stop; hostile requests; slow clients and timeouts; admission when full; per-user rate limit; 100 concurrent clients; disconnects; stop with active, stalled and queued requests |
| `ApiLinuxPeers.*` (Linux, root) | real other-uid peers: socket permissions, and credentials when permissions are wrong |
| `tests/check_api_schemas.py` | a live agent's responses against the schemas |
| `osie_fuzz_api` | libFuzzer over HTTP parsing, routing, validation and diagnosis |
| `packaging/agent/agent_systemd_test.sh` | under systemd: off by default, the `osie` group, socket and directory modes, root and member access, outsider refused, curl, no IP socket, stop with a stalled client, disabling again |

Measurements are in [`BENCHMARKS.md`](BENCHMARKS.md), "Local API".
