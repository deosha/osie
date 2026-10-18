#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# `osie remote` against a real OpenSSH server and a real osie-agent, with real
# OS users (Linux, as root, on a disposable machine or CI runner).
#
#   sudo tests/platform/remote_ssh_test.sh BUILD_DIR [--bench OUT_DIR]
#
# With --bench, tests/platform/remote_ssh_bench.py also runs after the
# scenarios and appends its measurements to OUT_DIR/remote_bench.jsonl.
#
# Needs: sshd, ssh, ssh-keygen (openssh-server, openssh-client), ss
# (iproute2), python3. Starts a private sshd on 127.0.0.1:2222 with its own
# host key and authorized-keys directory (no system SSH configuration is
# read or changed), an agent with the API enabled, and these users:
#   osie-alice, osie-bob  members of the API group (diagnostics tier)
#   osie-eve              not a member
#   root                  the agent's own user here (full tier)
# Everything it creates is removed on exit.
set -uo pipefail

BUILD=${1:?usage: remote_ssh_test.sh BUILD_DIR [--bench OUT_DIR]}
BENCH_OUT=""
[ "${2:-}" = --bench ] && BENCH_OUT=${3:?--bench needs OUT_DIR}
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 2; }
for t in sshd ssh ssh-keygen ss python3 runuser setpriv pgrep; do
  command -v "$t" >/dev/null || [ -x "/usr/sbin/$t" ] || { echo "missing $t" >&2; exit 2; }
done
SSHD=$(command -v sshd || echo /usr/sbin/sshd)

PORT=2222
GROUP=osie-remote-t
USERS="osie-alice osie-bob osie-eve"
# Not under /tmp: sshd's StrictModes refuses key files below a world-writable directory.
WORK=$(mktemp -d /run/osie-ssh-test-work.XXXXXX)
chmod 755 "$WORK"
RUNDIR=/run/osie-ssh-test
SOCK=$RUNDIR/agent.sock
PASS=0
FAIL=0
AGENT_PID=""
BLACKHOLE_PID=""

cleanup() {
  [ -n "$AGENT_PID" ] && kill "$AGENT_PID" 2>/dev/null && wait "$AGENT_PID" 2>/dev/null
  [ -n "$BLACKHOLE_PID" ] && kill "$BLACKHOLE_PID" 2>/dev/null
  [ -f "$WORK/sshd/sshd.pid" ] && kill "$(cat "$WORK/sshd/sshd.pid")" 2>/dev/null
  for u in $USERS; do pkill -u "$u" 2>/dev/null; userdel -r "$u" 2>/dev/null; done
  groupdel "$GROUP" 2>/dev/null
  rm -rf "$WORK" "$RUNDIR"
}
trap cleanup EXIT

ok() { PASS=$((PASS + 1)); echo "PASS  $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL  $*"; }
check() {
  local what=$1; shift
  if "$@"; then ok "$what"; else bad "$what (last command: exit ${RC:-?}; ${ERR:0:300})"; fi
}

# ------------------------------------------------------------------ setup ---
install -d -m 755 "$WORK/bin"
install -m 755 "$BUILD/osie" "$WORK/bin/osie"
install -m 755 "$BUILD/osie-agent" "$WORK/bin/osie-agent"
OSIE=$WORK/bin/osie

groupadd "$GROUP"
for u in $USERS; do
  useradd -m -s /bin/bash "$u"
  usermod -p '*' "$u"   # no password, but not a locked account (sshd refuses those)
done
usermod -aG "$GROUP" osie-alice
usermod -aG "$GROUP" osie-bob

# sshd: private host key, private authorized keys, Unix-socket forwarding on.
install -d -m 755 "$WORK/sshd" "$WORK/keys"
ssh-keygen -q -t ed25519 -N '' -f "$WORK/sshd/host_ed25519" -C test-host
ssh-keygen -q -t ed25519 -N '' -f "$WORK/sshd/other_host" -C other-host   # for the mismatch test
cat >"$WORK/sshd/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $WORK/sshd/host_ed25519
PidFile $WORK/sshd/sshd.pid
AuthorizedKeysFile $WORK/keys/%u
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
UsePAM no
AllowStreamLocalForwarding local
# OpenSSH (9.6) grants direct-streamlocal channels through the same permission
# as local TCP forwarding: with "AllowTcpForwarding no" (or "PermitOpen none")
# the forward to the agent socket is denied. See docs/REMOTE.md.
AllowTcpForwarding local
X11Forwarding no
PermitTTY no
LogLevel VERBOSE
EOF
mkdir -p /run/sshd
"$SSHD" -f "$WORK/sshd/sshd_config" -E "$WORK/sshd/log" || { echo "sshd failed"; cat "$WORK/sshd/log"; exit 1; }
for i in $(seq 50); do ss -ltnH "sport = :$PORT" | grep -q . && break; sleep 0.1; done
HOSTKEY=$(cut -d' ' -f1,2 "$WORK/sshd/host_ed25519.pub")
OTHERKEY=$(cut -d' ' -f1,2 "$WORK/sshd/other_host.pub")

home_of() { if [ "$1" = root ]; then echo "$WORK/root-home"; else getent passwd "$1" | cut -d: -f6; fi; }
install -d -m 700 "$WORK/root-home"
for u in $USERS root; do
  h=$(home_of "$u")
  install -d -m 700 -o "$u" "$h/.ssh"
  runuser -u "$u" -- ssh-keygen -q -t ed25519 -N '' -f "$h/.ssh/id_osie" -C "$u-test"
  runuser -u "$u" -- ssh-keygen -q -t ed25519 -N '' -f "$h/.ssh/id_unauthorized" -C "$u-wrong"
  install -m 644 -o root "$h/.ssh/id_osie.pub" "$WORK/keys/$u"
  # known_hosts populated by the harness from the server's real host key.
  echo "[127.0.0.1]:$PORT $HOSTKEY" >"$h/.ssh/known_hosts_osie"
  echo "[127.0.0.1]:$PORT $OTHERKEY" >"$h/.ssh/known_hosts_other"
  : >"$h/.ssh/known_hosts_empty"
  chown "$u" "$h/.ssh/"known_hosts_*
  chmod 600 "$h/.ssh/"known_hosts_*
done
KH_SUMS=$(for u in $USERS root; do md5sum "$(home_of "$u")/.ssh/"known_hosts_*; done)

# The agent: API on, group $GROUP, socket directory root:$GROUP 0750.
install -d -m 750 -o root -g "$GROUP" "$RUNDIR"
install -d -m 755 "$WORK/agent"
cat >"$WORK/agent/agent.conf" <<EOF
interval = 1s
sample_window = 200ms
api_enabled = true
api_socket = $SOCK
api_group = $GROUP
api_rate_limit = 1000
EOF
chmod 600 "$WORK/agent/agent.conf"
"$WORK/bin/osie-agent" --config "$WORK/agent/agent.conf" --allow-root >"$WORK/agent/log" 2>&1 &
AGENT_PID=$!
for i in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.1; done
sleep 2.5   # two collections
[ -S "$SOCK" ] || { echo "agent did not start"; cat "$WORK/agent/log"; exit 1; }

# A TCP listener that accepts and never speaks (for timeouts).
python3 - "$WORK/blackhole.port" <<'PY' &
import socket, sys, time
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(64)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
conns = []
while True:
    c, _ = s.accept(); conns.append(c)
PY
BLACKHOLE_PID=$!
for i in $(seq 50); do [ -s "$WORK/blackhole.port" ] && break; sleep 0.1; done
BH_PORT=$(cat "$WORK/blackhole.port")

# ---------------------------------------------------------------- helpers ---
# as_user USER CMD...: exec CMD as USER with its groups (setpriv execs, so a
# signal sent to the returned pid reaches CMD itself, unlike runuser or su).
as_user() { local u=$1; shift; setpriv --reuid="$u" --regid="$u" --init-groups "$@"; }
# r USER ARGS...: runs `osie remote ARGS` as USER with a clean environment;
# sets RC, OUT, ERR, MS.
r() {
  local u=$1; shift
  local h; h=$(home_of "$u")
  local t0 t1
  t0=$(date +%s%N)
  if [ "$u" = root ]; then
    env -i HOME="$h" PATH=/usr/bin:/bin:/usr/sbin USER=root "$OSIE" remote "$@" >"$WORK/out" 2>"$WORK/err"
  else
    as_user "$u" env -i HOME="$h" PATH=/usr/bin:/bin USER="$u" "$OSIE" remote "$@" >"$WORK/out" 2>"$WORK/err"
  fi
  RC=$?
  t1=$(date +%s%N)
  MS=$(( (t1 - t0) / 1000000 ))
  OUT=$(cat "$WORK/out"); ERR=$(cat "$WORK/err")
}
code_of() { python3 -c 'import json,sys; print(json.loads(sys.stdin.read())["remote_error"]["code"])' <<<"$OUT" 2>/dev/null; }
json_get() { python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print($1)" <<<"$OUT" 2>/dev/null; }
expect_transport() {  # NAME CODE
  if [ "$RC" -eq 3 ] && [ "$(code_of)" = "$2" ]; then ok "$1 -> $2 (exit 3)"; else bad "$1: expected $2/exit 3, got exit $RC: $OUT $ERR"; fi
}
add_profile() {  # USER NAME [extra add options]
  local u=$1 n=$2; shift 2
  local h; h=$(home_of "$u")
  r "$u" add "$n" "$u@127.0.0.1" --port "$PORT" --identity "$h/.ssh/id_osie" --known-hosts "$h/.ssh/known_hosts_osie" \
    --socket "$SOCK" --timeout 10 "$@"
  [ "$RC" -eq 0 ] || bad "add $n for $u: $ERR"
}
local_get() {  # USER PATH -> body via the agent socket directly, as USER
  runuser -u "$1" -- python3 - "$SOCK" "$2" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(("GET %s HTTP/1.1\r\nHost: l\r\n\r\n" % sys.argv[2]).encode())
d = b""
while True:
    b = s.recv(65536)
    if not b: break
    d += b
sys.stdout.buffer.write(d.split(b"\r\n\r\n", 1)[1])
PY
}
leftovers() {  # no ssh of the test users, no private directories anywhere
  local n=0
  for u in $USERS; do pgrep -u "$u" -x ssh >/dev/null && n=$((n + 1)); done
  ls -d /tmp/osie-remote-* 2>/dev/null | grep -q . && n=$((n + 1))
  [ "$n" -eq 0 ]
}

for u in $USERS root; do add_profile "$u" local; done

# --------------------------------------------------------------- scenarios ---
echo "== host keys verified, profiles, access tiers"
r osie-alice list --json
check "profile file is 0600 and parses" test "$(stat -c %a /home/osie-alice/.config/osie/remotes.conf)" = 600
r osie-alice status local
check "alice (member) status: ready, diagnostics tier" grep -q "access:  diagnostics" <<<"$OUT"
echo "$OUT" | sed 's/^/      /'
r osie-alice ask local "what is using the most CPU?" --json
check "alice ask --json: exit 0, an agent answer" test "$RC" -eq 0 -a "$(json_get 'd["schema_version"].split(".")[0]')" = "1"
r osie-alice ask local "what is using the most CPU?"
check "alice ask (text): exit 0" test "$RC" -eq 0
for sub in snapshot history capabilities; do
  r osie-alice "$sub" local --json
  check "alice $sub --json: exit 0" test "$RC" -eq 0 -a "$(json_get 'd["schema_version"].split(".")[0]')" = "1"
done
r osie-alice history local --json --limit 3 --metric cpu_busy_pct
check "history parameters pass through (returned <= 3)" test "$(json_get 'd["returned"]')" -le 3
r osie-alice history local --json --limit 0
check "history out-of-range: the agent's 400 passed through (exit 1)" test "$RC" -eq 1 -a "$(json_get 'd["error"]["code"]')" = invalid_request
r osie-alice capabilities local --json
REMOTE_CAPS=$OUT
LOCAL_CAPS=$(local_get osie-alice /v1/capabilities)
check "capabilities over SSH are byte-identical to the local socket" test "$REMOTE_CAPS" = "$LOCAL_CAPS"

echo "== a non-member"
r osie-eve status local --json
expect_transport "eve (non-member), default socket modes" remote_socket_unavailable
r osie-eve ask local "what is using the most CPU?" --json
expect_transport "eve ask, default socket modes" remote_socket_unavailable
# Socket permissions loosened by mistake: the kernel credentials still limit eve.
chmod 755 "$RUNDIR"; chmod 666 "$SOCK"
r osie-eve status local
check "eve with loosened modes: ready, health tier only (exit 0)" grep -q "access:  health only" <<<"$OUT"
r osie-eve ask local "what is using the most CPU?" --json
check "eve diagnose: the agent's 403 forbidden passed through (exit 1)" \
  test "$RC" -eq 1 -a "$(json_get 'd["error"]["code"]')" = forbidden
r osie-eve snapshot local --json
check "eve snapshot: 403 (exit 1)" test "$RC" -eq 1
chmod 750 "$RUNDIR"; chmod 660 "$SOCK"

echo "== root (the agent's user): full tier, as locally"
r root capabilities local --json
check "root capabilities: exit 0" test "$RC" -eq 0
r root snapshot local --json
ROOT_SNAP=$OUT
r osie-alice snapshot local --json
ALICE_SNAP=$OUT
check "root's snapshot shows paths of processes alice may not see (full tier)" python3 -c '
import json, sys, pwd
d = json.loads(sys.argv[1]); me = pwd.getpwnam("osie-alice").pw_uid
procs = [p for k in ("top_cpu", "top_memory") for p in d.get("processes", {}).get(k, [])]
sys.exit(0 if any(p.get("path") and p.get("uid") != me for p in procs) else 1)' "$ROOT_SNAP"
check "alice's snapshot withholds other users' paths" python3 -c '
import json, sys, pwd
d = json.loads(sys.argv[1]); me = pwd.getpwnam("osie-alice").pw_uid
procs = [p for k in ("top_cpu", "top_memory") for p in d.get("processes", {}).get(k, [])]
leaks = [p for p in procs if p.get("uid") != me and p.get("path")]
sys.exit(1 if leaks else 0)' "$ALICE_SNAP"
LOCAL_ROOT_ASK=$(python3 - "$SOCK" <<'PY'
import socket, sys
b = b'{"question":"what is using the most CPU?"}'
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(b"POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s" % (len(b), b))
d = b""
while True:
    x = s.recv(65536)
    if not x: break
    d += x
print(d.split(b"\r\n", 1)[0].decode())
PY
)
r root ask local "what is using the most CPU?" --json
check "root ask over SSH: 200 like the local socket ($LOCAL_ROOT_ASK)" test "$RC" -eq 0 -a "$LOCAL_ROOT_ASK" = "HTTP/1.1 200 OK"

echo "== sessions never cross users"
r osie-alice ask local "which process uses the most memory?" --session shared-1 --json
r osie-alice ask local "why is it using so much memory?" --session shared-1 --json
ALICE_FOLLOW=$(json_get 'd["status"]')
r osie-bob ask local "why is it using so much memory?" --session shared-1 --json
BOB_FOLLOW=$(json_get 'd["status"]')
echo "      alice follow-up status: $ALICE_FOLLOW; bob with the same session id: $BOB_FOLLOW"
check "bob's 'it' does not resolve to alice's subject" test "$BOB_FOLLOW" = needs_clarification -a "$ALICE_FOLLOW" != needs_clarification

echo "== transport failures"
r osie-alice add missing "osie-alice@127.0.0.1" --port $PORT --identity /home/osie-alice/.ssh/id_osie \
  --known-hosts /home/osie-alice/.ssh/known_hosts_osie --socket $RUNDIR/missing.sock
r osie-alice capabilities missing --json
expect_transport "missing remote socket" remote_socket_unavailable
r osie-alice add unknown "osie-alice@127.0.0.1" --port $PORT --identity /home/osie-alice/.ssh/id_osie \
  --known-hosts /home/osie-alice/.ssh/known_hosts_empty --socket $SOCK
r osie-alice status unknown --json
expect_transport "host key not in known_hosts" host_key_unknown
r osie-alice add changed "osie-alice@127.0.0.1" --port $PORT --identity /home/osie-alice/.ssh/id_osie \
  --known-hosts /home/osie-alice/.ssh/known_hosts_other --socket $SOCK
r osie-alice status changed --json
expect_transport "host key differs from known_hosts" host_key_mismatch
r osie-alice add wrongkey "osie-alice@127.0.0.1" --port $PORT --identity /home/osie-alice/.ssh/id_unauthorized \
  --known-hosts /home/osie-alice/.ssh/known_hosts_osie --socket $SOCK
r osie-alice status wrongkey --json
expect_transport "key not authorized" authentication_failed
r osie-alice add refused "osie-alice@127.0.0.1" --port 1 --known-hosts /home/osie-alice/.ssh/known_hosts_osie
r osie-alice status refused --json
expect_transport "connection refused" connection_failed
r osie-alice add noname "osie-alice@nonexistent.invalid" --known-hosts /home/osie-alice/.ssh/known_hosts_osie
r osie-alice status noname --json
expect_transport "unresolvable host" connection_failed
check "known_hosts files were never written" test "$KH_SUMS" = "$(for u in $USERS root; do md5sum "$(home_of "$u")/.ssh/"known_hosts_*; done)"
check "no default known_hosts was created" test ! -e /home/osie-alice/.ssh/known_hosts

echo "== deadline and cleanup"
r osie-alice add blackhole "osie-alice@127.0.0.1" --port "$BH_PORT" --known-hosts /home/osie-alice/.ssh/known_hosts_osie
r osie-alice status blackhole --json --timeout 2
expect_transport "server that never speaks, --timeout 2" timeout
echo "      elapsed ${MS} ms"
check "timeout bounded: 2 s <= elapsed < 3.5 s" test "$MS" -ge 1900 -a "$MS" -lt 3500
check "no ssh process or private directory left after the timeout" leftovers
for sig in TERM INT; do
  # setpriv execs: $! is osie itself (a shell function or runuser would sit in between).
  setpriv --reuid=osie-alice --regid=osie-alice --init-groups \
    env -i HOME=/home/osie-alice PATH=/usr/bin:/bin "$OSIE" remote status blackhole --json --timeout 30 \
    >"$WORK/sig.out" 2>&1 &
  P=$!
  sleep 1
  pgrep -u osie-alice -x ssh >/dev/null && SSH_UP=yes || SSH_UP=no
  T0=$(date +%s%N)
  kill -"$sig" "$P"
  wait "$P"; SRC=$?
  T1=$(date +%s%N)
  echo "      SIG$sig: exit $SRC after $(( (T1 - T0) / 1000000 )) ms (ssh was running: $SSH_UP)"
  check "SIG$sig: osie ends by the signal, ssh stopped, directory removed" \
    test "$SSH_UP" = yes -a "$SRC" -eq $((128 + $(kill -l "$sig"))) -a "$(leftovers && echo y)" = y
done

echo "== no OSIE TCP listener"
check "no TCP or UDP listener belongs to osie or osie-agent" sh -c "! ss -ltunpH | grep -q osie"
check "osie-agent has no IP socket at all" sh -c "! ss -tunapH | grep -q 'pid=$AGENT_PID,'"
check "nothing left behind by any test" leftovers

if [ -n "$BENCH_OUT" ]; then
  echo "== benchmarks"
  mkdir -p "$BENCH_OUT"
  python3 "$HERE/remote_ssh_bench.py" "$OSIE" osie-alice /home/osie-alice "$SOCK" "$BENCH_OUT" || bad "benchmark run"
  check "nothing left behind by the benchmarks" leftovers
fi

echo
echo "remote_ssh_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
