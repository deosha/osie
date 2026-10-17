# v0.3 Features 3 and 4: shared contracts

Frozen before implementation, against `main` at `dca2f48`, the
merge of Feature 2, the local diagnostic API. Feature 3 is secure remote
diagnostics over OpenSSH. Feature 4 is evidence provenance and historical
quality. Each is developed on its own branch and reviewed separately. This
document says what each may change, what neither may change, and how they
meet.

**Feature 3 transports Feature 4's evidence without reinterpreting it.**

## 1. The diagnostic JSON API (existing, owned by neither feature)

- **Specification.** The endpoints, envelopes and errors are those of
  [`API.md`](API.md) and `docs/schemas/api_*.schema.json`. The versioning is
  `/v1` and `schema_version` `"1.0"`.
- **Compatibility rules** ([`API.md`](API.md), "Compatibility rules"):
  - Within 1.x, fields and enum values may be added.
  - Clients ignore what they do not know.
  - Error `code` values are stable; `reason` values may be added.
  - Removing or renaming a field, changing a type or a meaning, or refusing a
    request that used to be accepted is a breaking change. It needs `/v2` and
    an explicit decision by the lead. **Neither feature makes one.**
- **Feature 4** may add fields and enum values to `api_diagnose`,
  `api_history`, `api_snapshot` and, if needed, `api_capabilities`. Every
  addition is documented in `API.md` and validated by
  `tests/check_api_schemas.py`.
- **Feature 3** adds no endpoint and no field to the agent's API. Its own
  output for transport failures has its own schema:
  `docs/schemas/remote_error.schema.json`.
- **CLI.** `osie ask --json` is additive only. The golden transcript and the
  frozen intent set must be unchanged. A change to answer text needs the
  lead's approval, and the PR must show it.

## 2. Evidence identifiers and provenance (Feature 4)

- **What exists.** Each observation carries `Provenance`, a native
  `source`, wall and monotonic times, a `Validity` and a `note`, but most of
  it is not exposed by the API.
- **Feature 4 decides the exposed form.** At minimum, each `evidence[]` item
  states:
  - its source;
  - its validity;
  - when it was observed;
  - for derived values (rates, deltas), the window it was derived over;
  - which snapshot (sequence number) it came from.
- **Temporal answers** state the window that was requested and the window
  actually observed: first and last point, span, number of points and gaps.
- **Identifiers** must be stable within one response, and meaningful only
  within it unless documented otherwise. They must not encode paths,
  arguments, environments or owners.

## 3. Request and session identity (existing)

- **Who the caller is** comes from the kernel peer credentials on the agent's
  socket (`SO_PEERCRED`). The access tier (`none`, `health`, `diagnostics`,
  `full`) is decided before the request is read.
- **Sessions** are keyed by (uid, session id) and never cross uids.
- **Feature 3** must keep this intact. Over SSH, the process connecting to
  the agent's socket is the remote `sshd` session process, running as the SSH
  login user. The agent therefore sees the remote login user's uid and groups,
  and applies its normal policy. Feature 3 never connects as another user,
  never uses a privileged helper, and never relaxes the socket's mode or
  group.
- **Session ids.** `osie remote ask --session ID` passes a session id
  through. A remote session belongs to the remote login user.

## 4. Snapshot and history ownership (existing)

- **The agent owns all of it.** The agent's runtime is the only sampler.
  Snapshots and the bounded in-memory `StateHistory` are published by it, and
  the API reads them (`AgentEvidence`) and never samples (`RefusingOs`).
- **Feature 4**:
  - may extend what history records, as long as it stays bounded and in
    memory, documented, with the overhead measured;
  - must not add a sampler, an on-demand read or persistent history.
- **Feature 3** has no access to snapshots or history except through the API.

## 5. API schema compatibility

- **Feature 4:** the additions in section 1. Existing fields keep their
  meaning; for example, `evidence[].value` stays a value of the snapshot the
  answer used.
- **Feature 3:**
  - `osie remote ask/snapshot/history/capabilities --json` writes the agent's
    response body to stdout byte for byte;
  - it never parses and re-serialises that body;
  - text mode may read only the fields it displays (`status`,
    `answer.text`, `limitations[].code/detail` and `error.*`), and ignores
    unknown fields.

  So whatever Feature 4 adds reaches remote clients unchanged.

## 6. Error classification

The remote client keeps two classes of failure apart:

| Class | Meaning | Exit code | Output |
|---|---|---|---|
| Diagnostic outcome | The agent answered: a 2xx answer, or a 4xx/5xx error envelope (`busy`, `stale_evidence`, `forbidden`, …) | 0 for 2xx; 1 for an API error | The agent's body, unchanged |
| Transport failure | No answer from the agent: OpenSSH not found, unknown host key, host key mismatch, authentication failed, name resolution or connection failed, timeout, remote agent socket missing or refused, malformed or oversized response | 3 | `remote_error` envelope, with a stable `code` such as `host_key_unknown`, `host_key_mismatch`, `authentication_failed`, `connection_failed`, `timeout`, `remote_socket_unavailable`, `protocol_error`, `ssh_unavailable` or `invalid_profile` |
| Usage | Bad arguments, or an invalid profile | 2 | message |

## 7. Authorization boundaries

- **Local agent:** unchanged. Kernel peer credentials, the socket mode and
  group, and the access tiers; redaction happens before reasoning.
- **Remote host.** OpenSSH authenticates the user, and verifies the server
  with `StrictHostKeyChecking=yes`. OSIE never disables that check, accepts
  new keys silently, writes `known_hosts`, or handles key material.
- **Feature 3: never through a shell**:
  - `ssh` is spawned with a fixed argument vector (`posix_spawn` or
    `execv`), never through a shell;
  - every profile field is validated against a strict grammar: no leading
    `-`, no whitespace or control characters, and absolute paths where paths
    are expected;
  - the destination follows `--`;
  - no user-supplied `-o` options are passed;
  - question text travels only inside the HTTP body sent through the
    forwarded socket. It never appears in any argument or remote command.
- **Local forwarding socket.** It lives in a fresh private directory (mode
  0700, created by `mkdtemp`), with `StreamLocalBindMask=0177`. It is
  removed on every exit path.
- **Profiles** live in a file owned by the user, mode 0600, in the user's
  config directory. They are written atomically and refused if the owner or
  mode is wrong. They contain no secrets: OpenSSH keys and agents stay
  OpenSSH's business.
- **Feature 4.** History and evidence carry no paths, arguments,
  environments or owners. Redaction for diagnostics-tier peers, and the
  `hidepid` rules, apply to every new field.

## 8. Performance limits

- **Feature 3** (client side; the agent is unchanged):
  - one `ssh` process per command;
  - an overall deadline (default 15 s, at most 120 s), covering connect,
    forward, request and response;
  - responses bounded at 4 MiB;
  - on every exit, including a signal, `ssh` is terminated and reaped, and
    the private directory removed;
  - no zombie processes, and no leaked descriptors or sockets.
- **Feature 4:**
  - no extra OS reads per collection, checked with `strace` or by counting
    refusals;
  - the history memory increase at the default configuration is measured,
    and kept under 25% unless the lead approves more;
  - p99 engine time per diagnosis at about 2,000 processes stays within
    1.5× of the Feature 2 baseline;
  - history queries stay O(history points × top_k).
- **Both:** the CPU, memory, threads and descriptors of an idle agent with
  the API enabled must not regress beyond noise.

## 9. File ownership

| Owner | Files |
|---|---|
| Feature 3 | `src/remote/**`, `include/osie/remote/**`, the `osie remote` dispatch in `src/cli/main.cpp`, `tests/unit/test_remote_*.cpp`, `tests/platform/test_remote_*` and SSH integration scripts, `tests/fuzz/fuzz_remote_*`, `docs/REMOTE.md`, `docs/schemas/remote_*.schema.json` |
| Feature 4 | `src/state/**`, `src/engine/**`, `src/reasoning/**`, `src/agent/runtime.cpp`, `src/api/evidence.cpp`, `src/api/api.cpp`, their headers, `docs/schemas/api_*.schema.json`, `tests/unit/test_api_truth.cpp`, `tests/unit/test_history*.cpp` and new evidence tests |
| Audit | `tests/audit/**` (its own scripts and fixtures), `docs/audit/**` |
| Shared | `CMakeLists.txt`, `.github/workflows/ci.yml`, `README.md`, `docs/SECURITY.md`, `docs/API.md`, `docs/ARCHITECTURE.md`. Edit them in separate commits, keep the hunks small, and expect the lead to integrate. Feature 3 adds a new "Remote diagnostics over SSH" section to `SECURITY.md`. Feature 4 edits the existing API, history and privacy sections. |

Neither implementation feature edits the other's files. If one needs a change
there, it asks the lead.

## 10. Out of scope

Both features, and the audit, leave out:
- an OSIE TCP listener;
- custom cryptography;
- MCP;
- fleet coordination;
- Kubernetes;
- persistent history or a "Time Machine";
- remediation or any state-changing endpoint;
- a new or retrained model;
- Features 5 to 8 (installation and cloud-init, MCP, demo, release gate);
- anything in v0.4 or later.

## Escalate to the lead (stop, don't work around)

- SSH needs local user authorization bypassed.
- A state-changing privilege is needed.
- A new collector adds measurable overhead.
- An API change would break existing clients.
- Data isolation can't be guaranteed.
- A high-severity finding can't be fixed.
- A hosted service is needed.
- An acceptance criterion can't be met.
