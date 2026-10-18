# `osie remote` measurements (v0.3 Feature 3)

Raw results are in `remote_bench.jsonl`, one JSON object per measurement.

## Method

`tests/platform/remote_ssh_test.sh BUILD_DIR --bench OUT_DIR`, run as root in
a disposable Ubuntu 24.04 container, runs the integration scenarios first and
then `tests/platform/remote_ssh_bench.py`. Setup:

- **Hardware.** Docker Desktop's Linux VM on an Apple-silicon Mac: aarch64,
  12 vCPUs, kernel 6.10.14-linuxkit. Other containers were running on the
  same host, so the numbers are indicative, not a controlled benchmark.
- **Software.** OpenSSH 9.6p1 (Ubuntu) as both client and server. sshd runs
  on 127.0.0.1:2222, so there is no network latency at all. The agent runs
  with the API enabled, `interval = 1s`, `api_rate_limit = 1000`, and the
  default `api_max_clients = 16` and `api_max_clients_per_user = 4`.
- **Build.** The default build type (RelWithDebInfo, `-O2`), with gcc 13.
- **Client.** Every command runs as `osie-alice`, a member of the API group,
  with a clean environment and its key file (no ssh-agent).

Answers (exit 0), API refusals (exit 1, counted by the agent's error code)
and transport failures (exit 3, counted by `remote_error` code) are counted
separately. Latency percentiles cover answered requests only.

## Results (from `remote_bench.jsonl`)

| Measurement | Result |
|---|---|
| SSH establishment: ssh spawned until the forwarding socket exists (key authentication, localhost), n=50 | p50 126 ms, p90 134 ms, max 137 ms |
| First request on a new connection (`/v1/health/ready`) | p50 42 ms, max 50 ms |
| Second request on the same connection (`/v1/capabilities`) | p50 0.5 ms; p90 42 ms |
| `osie remote status` (both requests), whole command | p50 177 ms, p99 218 ms |
| `osie remote ask --json`, whole command, n=50 | 50/50 answered; p50 170 ms, p99 195 ms |
| Local baseline: one `POST /v1/diagnose` on the agent socket (in-process client, same user), n=50 | p50 0.16 ms, p99 0.49 ms |
| Client resources per `osie remote ask` (wait4 rusage of osie, which includes the ssh it spawned and reaped) | max RSS 11.1 MB; CPU (user+sys) p50 38 ms, max 52 ms |
| 1 client in a loop, 10 s | 59 answered (5.9/s), p50 171 ms; no refusals or failures |
| 4 clients, 10 s | 252 answered (25.0/s), p50 158 ms; no refusals or failures |
| 16 clients, 10 s | 695 answered (68.3/s), p50 169 ms, p99 218 ms; **201 refused by the agent (429 `rate_limited`)**; **244 transport failures** (`connection_failed`, with ssh reporting `kex_exchange_identification: read: Connection reset by peer`) |
| Deadline, against a TCP server that never sends its SSH banner (3 runs each) | `--timeout 1`: 1006-1012 ms; `--timeout 2`: 2008-2015 ms; `--timeout 5`: 5014-5015 ms. Overshoot at most 15 ms. No ssh process or private directory left. |
| SIGTERM to `osie` while ssh waits for the banner, until osie has stopped ssh, removed its directory and exited (n=5) | p50 4.0 ms, max 5.9 ms. Nothing left. |

## Reading the results

- **SSH dominates.** The agent answers in well under a millisecond. Remotely,
  each command pays for process start, the SSH handshake and key
  authentication (about 125 ms here), and about 40 ms for the first request
  on a new connection. A second request on the same connection takes about
  0.5 ms at the median. The constant ~40 ms looks like a delayed-ACK
  interaction in OpenSSH's channel setup when no TTY is allocated, but this
  was not investigated further. It is OpenSSH's behaviour, not the agent's.
- **16 parallel clients of one user** exceed two limits that exist on
  purpose:
  - The agent's per-user admission limit (`api_max_clients_per_user = 4`)
    answers 429. These are counted as refusals, not successes.
  - sshd's default `MaxStartups 10:30:100` resets some unauthenticated
    connections at random. This is consistent with the
    `kex_exchange_identification` resets that were seen.

  Both behave as configured. Neither is a client defect.
- **Not measured:**
  - a real network link (only loopback);
  - macOS as the client against a Linux server;
  - an ssh-agent or a passphrase-protected key;
  - a jump host.
