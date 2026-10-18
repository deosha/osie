# Remote diagnostics over OpenSSH (`osie remote`, v0.3 Feature 3)

`osie remote` asks an `osie-agent` on another host the same questions that
local programs ask it, over the SSH access you already have:
- OSIE adds no network listener, remote helper, remote command or
  cryptography;
- the remote agent is unchanged.

```text
osie remote ask web1 "why is it slow?"
  │
  ├─ spawns: ssh -N -T -o BatchMode=yes -o StrictHostKeyChecking=yes ... \
  │            -L <private dir>/fwd.sock:/run/osie/agent.sock -- alice@web1
  │
  └─ AF_UNIX ─► <private dir>/fwd.sock ══ SSH ══► sshd (as alice) ─ AF_UNIX ─► /run/osie/agent.sock
     HTTP/1.1     (0700 dir, 0600 socket)            connects as the            agent: SO_PEERCRED → alice's
                                                     login user                 uid/groups → tier, redaction,
                                                                                per-uid sessions
```

The remote sshd session process connects to the agent's socket **as the SSH
login user**. The agent therefore authorises that user exactly as it would
locally:
- by kernel peer credentials, the socket's mode and group, and the access
  tiers of [`API.md`](API.md);
- the same redaction and per-uid sessions apply.

The connection is never made as another user, no privileged helper is
involved, and the socket's permissions are not changed. Each command uses
one `ssh` process. It speaks the agent's existing HTTP/JSON API over one or
two forwarded connections, then stops `ssh`.

## Setting it up

### On the remote host

1. Enable the API ([`API.md`](API.md), "Enabling it"):
   - set `api_enabled = true`;
   - set `api_group = osie`, and add the users who may diagnose to that
     group.
2. Let OpenSSH forward to Unix sockets for those users. With OpenSSH 9.6
   (tested), **the Unix-socket channel is granted through the same
   permission as local TCP forwarding**: `AllowTcpForwarding no`, or
   `PermitOpen none`, makes sshd deny it ("Received request to connect to
   path ..., but the request was denied"). A minimal grant:

   ```text
   # /etc/ssh/sshd_config.d/osie.conf
   Match Group osie
       AllowStreamLocalForwarding local
       AllowTcpForwarding local
   ```

   These are OpenSSH's defaults (`yes` for both), so many hosts need no
   change. `AllowTcpForwarding local` also lets these users forward TCP
   ports. That is OpenSSH's granularity, not OSIE's.

Group membership is read when the SSH session starts. A change takes effect
at the next connection, and every `osie remote` command is a new
connection.

| Remote login user | Tier the agent applies |
|---|---|
| member of `api_group` (primary or supplementary) | diagnostics (other users' executable paths withheld) |
| the agent's own user, or root | full, the same as locally |
| anyone else | cannot open the socket (`/run/osie` is 0750, the socket 0660): `remote_socket_unavailable` |
| anyone else, if the socket's modes were loosened by mistake | health only: diagnostics get the agent's 403 `forbidden` |

### On your machine

1. Requires OpenSSH client **8.7 or later**, for `SessionType`, `StdinNull`
   and `ForkAfterAuthentication`. Older clients fail with `ssh_unavailable`.
2. Put the remote host key in `known_hosts`. **OSIE never accepts, adds or
   updates a host key.** Verify the key out of band, for example with
   `ssh-keyscan` compared against the server's own `ssh-keygen -lf`, or
   provisioned by your configuration management.
3. Add a profile:

```bash
osie remote add web1 alice@web1.example.com
osie remote add db --port 2222 --identity ~/.ssh/id_osie --known-hosts ~/.ssh/known_hosts_osie \
    --socket /run/osie/agent.sock --timeout 20 ops@[2001:db8::7]   # the shell expands ~ before osie sees it
osie remote list
osie remote remove db
```

## Commands

| Command | Requests (one SSH connection) | `--json` output |
|---|---|---|
| `status NAME` | `GET /v1/health/ready`, `GET /v1/capabilities` | `remote_status` (both bodies embedded verbatim, with latencies) |
| `ask NAME "question" [--session ID] [--max-evidence-age-ms N]` | `POST /v1/diagnose` | the agent's body, byte for byte |
| `snapshot NAME` | `GET /v1/system/snapshot` | the agent's body, byte for byte |
| `history NAME [--window-seconds N] [--limit N] [--metric M]` | `GET /v1/system/history?...` | the agent's body, byte for byte |
| `capabilities NAME` | `GET /v1/capabilities` | the agent's body, byte for byte |
| `list` | none | `remote_list` |
| `add`, `remove` | none | none |

The commands that contact a remote host accept `--timeout S`, from 1 to
120 seconds. It defaults to the profile's timeout, else 15.

**`--json`.**
- The agent's response body is written to stdout exactly as received, with
  no trailing newline added. It is never parsed and re-serialised. So any
  field added to the API (for example by Feature 4) reaches you unchanged.
- The body is still validated first: it must be one JSON object, at most
  4 MiB, framed by `Content-Length`. Its `schema_version` must be any `1.x`:
  minor versions only add fields, which reach you unchanged. A different
  major version is refused (`protocol_error`).

**Text mode.**
- `ask` prints `answer.text`, then `status` unless it is `completed`, then
  `limitations[].code/detail`.
- API errors print `error.code`, `reason`, `message` and `retryable`.
- `snapshot`, `history` and `capabilities` print the body re-indented: only
  the whitespace between tokens changes.
- Nothing else is read, and unknown fields are ignored.

**Sessions.** `--session ID` is passed to the agent as `session_id`. The
session belongs to the remote login user. Another user who sends the same
id gets a separate, empty session (tested with real users:
`remote_ssh_test.sh`, "sessions never cross users").

## Exit status and errors

| Exit | Meaning | stdout with `--json` |
|---|---|---|
| 0 | the agent answered 2xx (for `status`: readiness answered 2xx) | the agent's body |
| 1 | the agent answered with an API error (`forbidden`, `busy`, `rate_limited`, `stale_evidence`, ...) or readiness 503 | the agent's body, unchanged |
| 2 | usage error, unknown profile, or invalid or unsafe profile file | a `remote_error` with `invalid_profile` (for profile errors) |
| 3 | transport failure: the agent did not answer | a `remote_error` envelope ([schema](schemas/remote_error.schema.json)) |
| 128+n | interrupted by signal n (SIGINT, SIGTERM, SIGHUP), after cleanup | a `remote_error` with `interrupted` |

```json
{"schema_version":"1.0","remote":"web1","remote_error":{"code":"host_key_unknown",
 "message":"The remote host key is not in known_hosts; ...","retryable":false,"ssh_exit_status":255,
 "ssh_stderr":"No ED25519 host key is known for web1 and you have requested strict checking.\nHost key verification failed."},
 "action_executed":false}
```

| `code` | When | Retryable |
|---|---|---|
| `host_key_unknown` | the host key is not in known_hosts | no |
| `host_key_mismatch` | the host key differs from known_hosts, or is revoked | no |
| `authentication_failed` | the server refused every key (batch mode: no password or passphrase prompt) | no |
| `connection_failed` | name resolution, connection refused or reset, or any other ssh failure before the forward was ready | yes |
| `timeout` | the overall deadline passed (in any stage), or ssh's connect timeout | yes |
| `remote_socket_unavailable` | the forwarded connection closed before any byte of a reply. OpenSSH prints the same `open failed` line whether the socket is missing, the user may not open it, or the server forbids the forward, so the message lists all three causes instead of guessing; the server's log tells them apart | yes |
| `protocol_error` | the reply was not a well-formed agent response (malformed, truncated, chunked, over 4 MiB, not JSON, no `schema_version`), or the forwarding socket was not ssh's | no |
| `ssh_unavailable` | no `ssh` found, or it rejects the options OSIE requires | no |
| `invalid_profile` | unknown profile, or the profile file is invalid or unsafe (exit 2) | no |
| `interrupted` | a signal stopped the command | yes |

Classification uses ssh's exit status (255 for its own failures) and its
standard error. That output is matched against fixed patterns, bounded to
16 KiB while read and 2 KiB when reported, and stripped of everything
outside printable ASCII, so a server banner cannot send escape sequences
to your terminal. The patterns are tested against OpenSSH's messages
(`RemoteClassify.*`) and against a real sshd (`remote_ssh_test.sh`).

## What runs, exactly

```text
ssh -N -T
    -o BatchMode=yes -o StrictHostKeyChecking=yes -o UpdateHostKeys=no -o CheckHostIP=no
    -o ExitOnForwardFailure=yes -o StreamLocalBindMask=0177 -o StreamLocalBindUnlink=no
    -o ForwardAgent=no -o ForwardX11=no -o ForwardX11Trusted=no -o Tunnel=no
    -o SessionType=none -o RemoteCommand=none -o RequestTTY=no -o StdinNull=yes -o EscapeChar=none
    -o PermitLocalCommand=no -o ControlMaster=no -o ControlPath=none -o ForkAfterAuthentication=no
    -o AddKeysToAgent=no -o LogLevel=ERROR -o ServerAliveInterval=5 -o ServerAliveCountMax=3
    -o ConnectTimeout=<remaining deadline, s>
    [-o IdentityFile=<profile identity> -o IdentitiesOnly=yes]
    [-o UserKnownHostsFile=<profile known_hosts>]
    [-p <port>]
    -L <private dir>/fwd.sock:<profile socket>
    -- <user@host>
```

(`RemoteSsh.GoldenArgv*` asserts this vector exactly.)

**The command line.**
- No shell is involved. On macOS `ssh` is started with `posix_spawn`, with
  `POSIX_SPAWN_CLOEXEC_DEFAULT`. On Linux it is started with `fork` and
  `execve`, so that the child can ask for `PR_SET_PDEATHSIG`; the child
  closes every descriptor above stderr before exec.
- The question never appears in it. It travels only inside the JSON body,
  through the forwarded socket.
- Options on the command line take precedence over `ssh_config`, so a
  user's or the system's configuration cannot turn off any of the forced
  ones.
- **`ClearAllForwardings` is not used**, because it would also clear OSIE's
  own `-L`. So `LocalForward`, `RemoteForward` or `DynamicForward` lines in
  your `ssh_config` that match the host are set up too, for the few
  seconds the command lasts.
- Your configuration is otherwise honoured, by design: host aliases,
  `ProxyJump`, `User`, `Port`, keys and `IdentityAgent`. `IdentityFile`
  lines from it are added to the profile's.

**Where `ssh` comes from.**
- The first executable `ssh` in the absolute entries of `$PATH`. Relative
  entries such as `.` are skipped. If there is none, `/usr/bin/ssh`.
- **`OSIE_SSH`**, if set, must be an absolute path to an executable file,
  and is used instead. It exists for the tests (a stand-in `ssh`). It is
  not a privilege boundary: `osie` runs as the invoking user, who can run
  any program anyway.
- In a set-id context (never how `osie` is installed), neither variable is
  read.

**The forwarding socket.**
- It lives in a fresh directory, `osie-remote-<96 random bits>`, created with
  `mkdirat` under the first usable one of:
  - `$XDG_RUNTIME_DIR`;
  - `$TMPDIR`;
  - `/tmp`.
- To be usable, a candidate must be:
  - an absolute path;
  - made of `[A-Za-z0-9/._-]`, because OpenSSH expands `%` and `${}` in
    forwarding paths, and `:` would split `-L`;
  - short enough for `sun_path`;
  - trusted by the same descriptor-based walk as the agent's configuration:
    owned by root or you, and not writable by others unless sticky.
- The directory is opened with `O_NOFOLLOW`, set to 0700 and checked on its
  descriptor (owner, mode). OpenSSH creates the socket with
  `StreamLocalBindMask=0177`, and never replaces an existing path
  (`StreamLocalBindUnlink=no`).
- Before use, the socket must be a socket owned by you. On connection, its
  kernel peer credentials must be your uid and, where the kernel reports
  it, the pid of the `ssh` that OSIE spawned. Otherwise: `protocol_error`.
- Only you (and root) can create anything in the directory, so nothing can
  be substituted between these checks.

**Deadline and cleanup.** One deadline (`--timeout`) covers:
- spawning `ssh`;
- connecting and authenticating (waiting for the socket while watching for
  `ssh` to exit);
- each connection;
- sending;
- receiving.

On every exit path:
- `ssh`'s process group, which includes anything it started such as a
  `ProxyCommand`, gets `SIGTERM`, then `SIGKILL` after 500 ms;
- `ssh` is reaped, and every descriptor is closed;
- the directory is removed with its contents.

The exit paths include success, errors, timeouts, and `SIGINT`, `SIGTERM`
or `SIGHUP` sent to `osie`. After a signal, `osie` cleans up and then
re-raises the signal.

## Security properties and limits

See also [`SECURITY.md`](SECURITY.md), "Remote diagnostics over SSH".

- **Host authenticity.**
  - `StrictHostKeyChecking=yes`, `UpdateHostKeys=no` and `CheckHostIP=no`:
    the host key must already be known, and OSIE never writes `known_hosts`.
    The integration test checks that the files are byte-identical after
    every scenario.
- **User authentication** is OpenSSH's. OSIE stores no secret:
  - profiles hold a destination, a port, paths and a timeout;
  - `BatchMode=yes`: a key that needs a passphrase works only through
    `ssh-agent`.
- **Authorization** is the remote agent's, on the login user's kernel
  credentials. The remote side cannot raise it, and the client cannot
  choose it.
- **Profiles.**
  - Location: `$XDG_CONFIG_HOME/osie/remotes.conf`, else
    `~/.config/osie/remotes.conf`.
  - Ownership and modes: the file is owned by you with mode 0600 or
    stricter, and its directory is owned by you with mode 0700. Every
    directory above them must be trusted. Anything else is refused, and so
    are symbolic links, FIFOs and files over 64 KiB.
  - Writes: a temporary file (`O_EXCL`, 0600) is written, fsynced and
    renamed, then the directory is fsynced, all under a `flock` on the
    directory (tested with 32 concurrent writers).
  - Field grammar:
    - name `[A-Za-z0-9._-]{1,64}`, starting with a letter or digit;
    - destination `[user@]host`: a POSIX-style user name, and a hostname,
      IPv4 address or bracketed IPv6 address;
    - port 1-65535;
    - remote socket: absolute, at most 100 bytes, `[A-Za-z0-9/._-]`;
    - identity and known_hosts paths: absolute, `[A-Za-z0-9/._+@-]`;
    - timeout 1-120 s.
  - No field may start with `-` or contain whitespace, quotes, `%`, `$`,
    `~` or shell metacharacters.
  - An IPv6 address is passed to ssh without its brackets.
- **Untrusted replies.** The remote side may be compromised. Its replies are
  parsed strictly and bounded:
  - picohttpparser, `Content-Length` framing only, at most 32 headers in
    16 KiB, and 4 MiB in all;
  - strict UTF-8 JSON, depth at most 64, no duplicate keys, and an agent
    `schema_version` of `1.x`;
  - fuzzed by `osie_fuzz_remote`.
- **Limits.**
  - On macOS, an `osie` killed with `SIGKILL` (which cannot be caught)
    leaves its `ssh` running and the directory in place, until the
    connection dies (`ServerAliveInterval`). On Linux, `PR_SET_PDEATHSIG`
    stops that `ssh`.
  - The remote socket path in a profile is trusted as the user typed it. A
    wrong path reaches whatever listens there, with the user's own SSH
    rights; a non-agent reply is refused as `protocol_error`.
  - Evaluated with OpenSSH only, as client and server. Dropbear and other
    SSH implementations were not evaluated.

## Performance

These are loopback measurements in a Linux container (method and raw data
in [`ml/experiments/results/remote/`](../ml/experiments/results/remote/README.md)):

| What | Time |
|---|---|
| SSH establishment, key authentication | p50 126 ms |
| `osie remote ask`, whole command | p50 170 ms |
| One diagnosis on the local socket | p50 0.16 ms |
| A second request on the same SSH connection | p50 0.5 ms |
| The first request on a connection | about 40 ms |

- Each command holds about 11 MB maximum RSS (`osie` and its `ssh`) and uses
  about 40 ms of CPU.
- With 16 parallel clients of one user:
  - the agent's per-user limit answers some requests with 429;
  - sshd's `MaxStartups` resets some connections (`connection_failed`).

  Both are counted separately from answers.
- The deadline overshoots by at most 15 ms.
- `SIGTERM` cleanup takes about 4 ms.

## Tests

| Test | Covers |
|---|---|
| `RemoteProfile.*` | field grammar against injection strings (`-oProxyCommand=`, `$(...)`, backticks, quotes, newlines, `%h`, `${HOME}`, 5000-byte values), the strict file format, modes, symlinks, FIFOs, size, 32 concurrent writers |
| `RemoteSsh.*` | golden argument vectors, structural invariants (forced options, `--`, no `ClearAllForwardings`, no `/dev/null` known_hosts), injection in every field, `OSIE_SSH` and `$PATH` lookup |
| `RemoteHttp.*` | response framing, truncation at every byte, oversize, chunked, 1xx, duplicates, non-agent replies, the JSON reader, byte-preserving re-indentation, request bodies, history parameters, text renderers |
| `RemoteClassify.*` | OpenSSH stderr fixtures for every code; the envelope |
| `RemoteTransport.*`, `RemoteCli.*` | a stand-in `ssh` with a real agent API server: byte-for-byte bodies, deadline in each stage, SIGKILL after an ignored SIGTERM, process group, no zombie, descriptor or directory left, cancellation, signals to `osie`, exit codes |
| `osie_fuzz_remote` | libFuzzer over the response parser, JSON reader, re-indenter, renderers and classifier |
| `tests/platform/test_remote_schemas.py` | `remote_error`, `remote_status` and `remote_list` outputs against their schemas |
| `tests/platform/remote_ssh_test.sh` (Linux, root) | real sshd, real users and the real agent: tiers, 403 pass-through, root's full tier, session isolation, host key unknown and changed, authentication, refused and unresolvable hosts, missing socket, timeouts and signals with cleanup checks, known_hosts never written, no TCP listener |
