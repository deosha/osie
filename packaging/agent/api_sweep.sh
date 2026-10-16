#!/usr/bin/env bash
# Concurrency sweep of the local API on Linux (docs/BENCHMARKS.md, "Local API:
# concurrency"). Runs the agent unprivileged with N extra sleeping processes
# (a high process count), reloads its configuration every 2 s, and for each
# concurrency level records the clients' latency and throughput together with
# the agent's own CPU time, RSS, threads, open descriptors and server-side
# timings (queue wait, engine wait, engine time; from SIGUSR1).
#
#   packaging/agent/api_sweep.sh <osie-agent> <osie_api_bench> [N=2000] [SECONDS=10] [LEVELS=1,4,8,16,32,100]
#
# One user plays every client here, so api_max_clients_per_user is raised to
# 16 for the sweep; with the default (4) one user is capped at 4 in flight.
set -euo pipefail
agent="${1:?osie-agent}"; bench="${2:?osie_api_bench}"; n="${3:-2000}"; secs="${4:-10}"; levels="${5:-1,4,8,16,32,100}"
work=$(mktemp -d /tmp/osapisweep.XXXXXX); chmod 700 "$work"
sock="$work/agent.sock"
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; [[ -n "${hup:-}" ]] && kill "$hup" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
for _ in $(seq 1 "$n"); do sleep 100000 & pids+=($!); done
printf 'interval = 5s\nsample_window = 500ms\napi_enabled = true\napi_socket = %s\napi_rate_limit = 10000\napi_max_clients_per_user = 16\n' \
  "$sock" > "$work/a.conf"; chmod 600 "$work/a.conf"
"$agent" --config "$work/a.conf" --report > "$work/report.json" 2> "$work/agent.err" &
apid=$!
for _ in $(seq 1 300); do [[ -S $sock ]] && break; sleep 0.05; done
sleep 1.5  # first collection
( while true; do sleep 2; kill -HUP $apid 2>/dev/null || exit 0; done ) & hup=$!

proc() {  # cpu_s rss_kb hwm_kb threads fds
  python3 - "$apid" <<'PY'
import os, sys
pid = sys.argv[1]
st = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
tick = os.sysconf("SC_CLK_TCK")
cpu = (int(st[11]) + int(st[12])) / tick
kv = {l.split(":")[0]: l.split(":")[1].strip() for l in open(f"/proc/{pid}/status")}
print(cpu, kv["VmRSS"].split()[0], kv["VmHWM"].split()[0], kv["Threads"], len(os.listdir(f"/proc/{pid}/fd")))
PY
}
api_stats() { kill -USR1 $apid; sleep 0.3; grep '^{"api"' "$work/agent.err" | tail -1; }

read -r c0 r0 h0 t0 f0 <<<"$(proc)"
echo "{\"idle\":{\"processes_on_host\":$(ls /proc | grep -c '^[0-9]'),\"rss_kb\":$r0,\"threads\":$t0,\"fds\":$f0}}"
IFS=, read -ra lv <<<"$levels"
for c in "${lv[@]}"; do
  read -r ca ra ha ta fa <<<"$(proc)"
  line=$("$bench" "$sock" --sweep "$c" --seconds "$secs")
  read -r cb rb hb tb fb <<<"$(proc)"
  cpu_share=$(python3 -c "print(round(($cb-$ca)/$secs, 3))")
  echo "{\"level\":$c,\"client\":$line,\"agent\":{\"cpu_share_of_one_core\":$cpu_share,\"rss_kb\":$rb,\"peak_rss_kb\":$hb,\"threads\":$tb,\"fds\":$fb},\"server\":$(api_stats)}"
done
# Shutdown with 100 clients active.
"$bench" "$sock" --sweep 100 --seconds 30 >/dev/null 2>&1 &
bpid=$!
sleep 3
t1=$(python3 -c 'import time; print(time.monotonic())'); kill -TERM $apid; wait $apid || true
t2=$(python3 -c 'import time; print(time.monotonic())')
kill $bpid 2>/dev/null || true; wait $bpid 2>/dev/null || true
echo "{\"shutdown_with_100_clients_ms\":$(python3 -c "print(round(($t2-$t1)*1000))")}"
echo "{\"report\":$(cat "$work/report.json")}"
