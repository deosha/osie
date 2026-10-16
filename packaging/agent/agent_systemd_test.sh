#!/usr/bin/env bash
# Integration test of the osie-agent systemd service. Run as root on a host
# (or privileged container) whose PID 1 is systemd.
#
# usage: packaging/agent/agent_systemd_test.sh <osie_*.deb>
set -euo pipefail
deb="${1:?path to the osie .deb}"
fail() { echo "AGENT FAIL: $*" >&2; journalctl -u osie-agent --no-pager -n 40 >&2 || true; exit 1; }
pass() { echo "ok - $*"; }
unit=osie-agent

wait_state() {  # wait_state <active|inactive|failed> <seconds>
  for _ in $(seq 1 $(( $2 * 10 ))); do
    [[ "$(systemctl is-active $unit || true)" == "$1" ]] && return 0
    sleep 0.1
  done
  return 1
}

dpkg -i "$deb" >/dev/null
systemctl daemon-reload

# A minimal HTTP/1.1 client over a Unix domain socket (python3 is everywhere
# this runs; curl is used too where installed). Prints "STATUS BODY", or
# "CONNECT <errno>" when the socket cannot be opened.
cat > /tmp/osie_uds.py <<'PY'
import socket, sys
path, method, target = sys.argv[1:4]
body = sys.argv[4] if len(sys.argv) > 4 else ""
s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
try:
    s.connect(path)
except OSError as e:
    print("CONNECT", e.errno); sys.exit(0)
req = f"{method} {target} HTTP/1.1\r\nHost: localhost\r\n"
if body or method == "POST":
    req += f"Content-Type: application/json\r\nContent-Length: {len(body.encode())}\r\n"
s.sendall((req + "\r\n" + body).encode())
data = b""
while True:
    c = s.recv(65536)
    if not c: break
    data += c
head, _, b = data.partition(b"\r\n\r\n")
print(head.split(b" ")[1].decode(), b.decode())
PY
chmod 0644 /tmp/osie_uds.py
sock=/run/osie/agent.sock
uds() { local user=$1; shift; su -s /bin/sh "$user" -c "python3 /tmp/osie_uds.py $(printf "'%s' " "$sock" "$@")"; }

# 1. Installed, but disabled and not running.
[[ "$(systemctl is-enabled $unit 2>/dev/null || true)" == "disabled" ]] || fail "unit should be installed disabled"
[[ "$(systemctl is-active $unit || true)" == "inactive" ]] || fail "unit must not start on install"
pass "installed disabled and inactive"

# 2. Starts (Type=notify: active only after the first collection), unprivileged.
systemctl start $unit
wait_state active 30 || fail "did not become active"
pid=$(systemctl show -p MainPID --value $unit)
[[ "$pid" -gt 0 ]] || fail "no main pid"
uid=$(awk '/^Uid:/ {print $2}' /proc/$pid/status)
[[ "$uid" != "0" ]] || fail "runs as root"
grep -q "^CapEff:[[:space:]]*0000000000000000" /proc/$pid/status || fail "has effective capabilities"
grep -q "^CapPrm:[[:space:]]*0000000000000000" /proc/$pid/status || fail "has permitted capabilities"
grep -q "^NoNewPrivs:[[:space:]]*1" /proc/$pid/status || fail "NoNewPrivs not set"
pass "running as uid $uid with no capabilities and NoNewPrivs"
[[ ! -e $sock ]] || fail "the local API must be off by default"
getent group osie >/dev/null || fail "the package did not create the osie group"
pass "local API off by default; osie group present"

# 3. No network: no IP sockets, only AF_UNIX (notify) and AF_NETLINK (interface counters).
sleep 2
socks=$(find /proc/$pid/fd -lname 'socket:*' 2>/dev/null | xargs -r -n1 readlink | sed 's/socket:\[\(.*\)\]/\1/' || true)
for inode in $socks; do
  if grep -qs " $inode " /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6 /proc/net/raw /proc/net/raw6; then
    fail "IP socket inode $inode is open"
  fi
done
pass "no IP sockets ($(echo $socks | wc -w) local sockets)"

# 4. Nothing written: the service cannot write the filesystem (ProtectSystem=strict) and writes nothing to /tmp.
marker=$(mktemp); sleep 1
written=$(find / -xdev \( -path /proc -o -path /sys -o -path /run -o -path /var/log -o -path /var/lib/systemd \) -prune \
          -o -type f -user "$uid" -newer "$marker" -print 2>/dev/null | head -5 || true)
[[ -z "$written" ]] || fail "files written by the agent: $written"
pass "no files written by the agent"

# 5. Reload (SIGHUP) with a valid config, then an invalid one (kept running, old config kept).
mkdir -p /etc/osie
printf 'interval = 1s\nsample_window = 200ms\n' > /etc/osie/agent.conf; chmod 0644 /etc/osie/agent.conf
systemctl reload $unit; sleep 2
journalctl -u $unit --no-pager | grep -q "configuration reloaded" || fail "reload not applied"
printf 'interval = 1s\nlisten = 0.0.0.0:80\n' > /etc/osie/agent.conf
systemctl reload $unit; sleep 2
journalctl -u $unit --no-pager | grep -q "keeping the current configuration" || fail "invalid reload not reported"
[[ "$(systemctl is-active $unit)" == "active" ]] || fail "invalid reload stopped the service"
pass "reload: valid applied, invalid rejected while running"

# 6. Repeated restarts and stop time.
printf 'interval = 1s\nsample_window = 200ms\n' > /etc/osie/agent.conf
for i in 1 2 3 4 5; do
  systemctl restart $unit
  wait_state active 30 || fail "restart $i did not become active"
done
t0=$(date +%s%N); systemctl stop $unit; t1=$(date +%s%N)
stop_ms=$(( (t1 - t0) / 1000000 ))
[[ "$(systemctl is-active $unit || true)" == "inactive" ]] || fail "did not stop"
logged=0
for _ in $(seq 1 30); do journalctl -u $unit --no-pager | grep -q "stopped in" && { logged=1; break; }; sleep 0.2; done
[[ $logged -eq 1 ]] || fail "no clean stop logged"
[[ $stop_ms -lt 5000 ]] || fail "stop took ${stop_ms} ms"
pass "5 restarts; stop in ${stop_ms} ms (clean)"

# 7a. Local API: enabled explicitly, for root and members of the osie group only.
id apiuser >/dev/null 2>&1 || useradd -m -G osie apiuser
id outsider >/dev/null 2>&1 || useradd -m outsider
printf 'interval = 1s\nsample_window = 200ms\napi_enabled = true\napi_group = osie\n' > /etc/osie/agent.conf
systemctl restart $unit
wait_state active 30 || fail "did not start with the API enabled"
for _ in $(seq 1 50); do [[ -S $sock ]] && break; sleep 0.1; done
[[ -S $sock ]] || fail "no API socket"
[[ "$(stat -c '%a %G' /run/osie)" == "750 osie" ]] || fail "runtime directory is $(stat -c '%a %G' /run/osie), want 750 osie"
[[ "$(stat -c '%a %G' $sock)" == "660 osie" ]] || fail "socket is $(stat -c '%a %G' $sock), want 660 osie"
pass "API socket $sock: mode 660, group osie, in a 750 directory"
r=$(uds root GET /v1/health/live); [[ $r == "200 "* ]] || fail "live: $r"
for _ in $(seq 1 50); do r=$(uds root GET /v1/health/ready); [[ $r == "200 "* ]] && break; sleep 0.2; done
[[ $r == "200 "*'"status":"ready"'* ]] || fail "ready: $r"
r=$(uds apiuser POST /v1/diagnose '{"question":"what is using the most CPU?","request_id":"ci-1"}')
[[ $r == "200 "*'"request_id":"ci-1"'*'"action_executed":false'* ]] || fail "diagnose as a member of osie: ${r:0:300}"
r=$(uds apiuser POST /v1/diagnose '{"question":"kill the busiest process"}')
[[ $r == "200 "*'"status":"refused"'* ]] || fail "action not refused: ${r:0:300}"
r=$(uds outsider GET /v1/health/live); [[ $r == "CONNECT 13" ]] || fail "a user outside the osie group could connect: $r"
pass "API: root and osie members served; actions refused; other users cannot connect"
if command -v curl >/dev/null; then
  code=$(curl -s -o /dev/null -w '%{http_code}' --unix-socket $sock http://localhost/v1/health/live)
  [[ $code == 200 ]] || fail "curl live: $code"
  code=$(curl -s -o /dev/null -w '%{http_code}' --unix-socket $sock -H 'Content-Type: application/json' \
         -d '{"question":"Why is my CPU usage high?"}' http://localhost/v1/diagnose)
  [[ $code == 200 ]] || fail "curl diagnose: $code"
  pass "documented curl commands work"
fi
pid=$(systemctl show -p MainPID --value $unit)
for inode in $(find /proc/$pid/fd -lname 'socket:*' 2>/dev/null | xargs -r -n1 readlink | sed 's/socket:\[\(.*\)\]/\1/'); do
  grep -qs " $inode " /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6 && fail "IP socket with the API enabled"
done
# Stop with a client stalled mid-request: bounded, and the socket is removed.
python3 - "$sock" <<'PY' &
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"POST /v1/diagnose HTTP/1.1\r\nHost: l\r\n"); time.sleep(8)
PY
slow=$!
sleep 0.5
t0=$(date +%s%N); systemctl stop $unit; t1=$(date +%s%N)
kill $slow 2>/dev/null || true
api_stop_ms=$(( (t1 - t0) / 1000000 ))
[[ $api_stop_ms -lt 6000 ]] || fail "stop with an active request took ${api_stop_ms} ms"
[[ ! -e /run/osie ]] || fail "/run/osie left behind"
pass "stop with an active API request: ${api_stop_ms} ms; socket directory removed"
# Disabling it again removes the socket.
printf 'interval = 1s\nsample_window = 200ms\n' > /etc/osie/agent.conf
systemctl start $unit; wait_state active 30 || fail "restart without the API"
[[ ! -e $sock ]] || fail "socket present after disabling the API"
systemctl stop $unit
pass "API disabled again: no socket"

# 7. Invalid configuration: fails with exit 2 and is not restarted in a loop.
printf 'interval = 1ms\n' > /etc/osie/agent.conf
systemctl reset-failed $unit || true
systemctl start $unit || true
sleep 7
[[ "$(systemctl show -p NRestarts --value $unit)" == "0" ]] || fail "restarted on a configuration error"
[[ "$(systemctl show -p ExecMainStatus --value $unit)" == "2" ]] || fail "expected exit status 2"
pass "invalid configuration: exit 2, no restart loop"

# 8. Security exposure as rated by systemd (informational).
systemd-analyze security $unit --no-pager 2>/dev/null | tail -1 || true

# 9. Removing the package while the agent runs stops it and removes the unit.
rm -f /etc/osie/agent.conf
systemctl reset-failed $unit || true
systemctl start $unit
wait_state active 30 || fail "did not start before removal test"
dpkg -r osie >/dev/null
sleep 1
pgrep -x osie-agent >/dev/null && fail "agent still running after package removal"
[[ ! -e /usr/bin/osie-agent && ! -e /usr/lib/systemd/system/osie-agent.service && ! -e /lib/systemd/system/osie-agent.service ]] \
  || fail "files left after removal"
pass "package removal stops the agent and leaves no files"
echo "AGENT SYSTEMD TEST OK"
