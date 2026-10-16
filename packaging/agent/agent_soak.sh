#!/usr/bin/env bash
# Prolonged-run check of osie-agent under a high process count with churn.
# Runs the agent for DURATION seconds next to N sleeper processes, CHURN of
# which are replaced every second. Every 10 s it samples the agent's RSS
# and, where the macOS `heap` tool is available, its live heap bytes.
#
# The history (120 points) fills within the first third of the run. The
# check compares the median of the last third with the median of the middle
# third: growth of more than 1 MB fails. It uses live heap bytes when they
# are available, and RSS otherwise. (macOS keeps freed malloc pages resident
# lazily, so its RSS swings by several MB between samples without any
# growth in live memory. `heap` briefly suspends the agent while it reads
# it, so some ticks are skipped on macOS; the counts are not a timing result.)
#
# usage: packaging/agent/agent_soak.sh <osie-agent> [DURATION=600] [N=2000] [CHURN=50] [INTERVAL=1s]
set -euo pipefail
agent="${1:?path to osie-agent}"
duration="${2:-600}"; n="${3:-2000}"; churn="${4:-50}"; interval="${5:-1s}"
work=$(mktemp -d); chmod 700 "$work"
printf 'interval = %s\nsample_window = 200ms\nhistory_capacity = 120\n' "$interval" > "$work/agent.conf"
chmod 600 "$work/agent.conf"

pids=()
cleanup() {
  [[ -n "${churner:-}" ]] && kill "$churner" 2>/dev/null || true
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  pkill -P $$ sleep 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

for _ in $(seq 1 "$n"); do sleep 100000 & pids+=($!); done
# Churn: replace CHURN short-lived processes every second (new PIDs, new start times).
( while true; do
    for _ in $(seq 1 "$churn"); do sleep 0.$((RANDOM % 9 + 1)) & done
    sleep 1
  done ) & churner=$!

"$agent" --config "$work/agent.conf" --duration "$duration" --report > "$work/report.json" 2> "$work/agent.log" &
apid=$!
has_heap=0; command -v heap >/dev/null 2>&1 && has_heap=1
echo "t_s,rss_kb,heap_bytes"
t=0; series=()
while kill -0 "$apid" 2>/dev/null; do
  r=$(ps -o rss= -p "$apid" 2>/dev/null | tr -d ' ' || true)
  h=""
  if (( has_heap )); then
    h=$(heap "$apid" 2>/dev/null | sed -n 's/^All zones: [0-9]* nodes (\([0-9]*\) bytes).*/\1/p' | head -1 || true)
  fi
  if [[ -n "$r" ]]; then
    echo "$t,$r,$h"
    if (( has_heap )) && [[ -n "$h" ]]; then series+=("$((h / 1024))"); else series+=("$r"); fi
  fi
  sleep 10; t=$((t + 10))
done
status=0; wait "$apid" || status=$?
if (( status != 0 )); then
  echo "SOAK FAIL: osie-agent exited with status $status"; tail -20 "$work/agent.log"; exit 1
fi
cat "$work/report.json"
median() { printf '%s\n' "$@" | sort -n | awk '{v[NR]=$1} END {print v[int((NR+1)/2)]}'; }
n3=$(( ${#series[@]} / 3 ))
(( n3 >= 2 )) || { echo "SOAK FAIL: run too short to judge"; exit 1; }
mid=$(median "${series[@]:n3:n3}")
last=$(median "${series[@]:$((2 * n3))}")
what=$([[ $has_heap -eq 1 ]] && echo "live heap" || echo "RSS")
echo "$what median: middle third ${mid} kB, last third ${last} kB"
(( last <= mid + 1024 )) || { echo "SOAK FAIL: $what grew"; exit 1; }
echo "SOAK OK"
