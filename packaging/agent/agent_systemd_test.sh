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
