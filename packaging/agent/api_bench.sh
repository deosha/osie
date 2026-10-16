#!/usr/bin/env bash
# Measures the local API's overhead and performance (docs/BENCHMARKS.md,
# "Local API"). Runs unprivileged; uses a private temporary socket directory.
#
#   packaging/agent/api_bench.sh <osie-agent> <osie_api_bench> [IDLE_SECONDS=120]
#
# Prints JSON lines:
#   idle_no_api, idle_api       --report of an idle agent without / with the API
#   startup                     ms from exec to the first 200 from /v1/health/ready
#   osie_api_bench phases       sequential latency, concurrent throughput, burst
#   under_load                  --report of the agent that served the load (peak RSS)
#   shutdown_under_load         ms from SIGTERM to exit with 16 clients active
set -euo pipefail
agent="${1:?osie-agent}"; bench="${2:?osie_api_bench}"; idle="${3:-120}"
work=$(mktemp -d /tmp/osapibench.XXXXXX); chmod 700 "$work"
trap 'rm -rf "$work"' EXIT
sock="$work/agent.sock"
conf() {  # conf <file> <api: true|false>
  printf 'interval = 5s\nsample_window = 500ms\napi_enabled = %s\napi_socket = %s\napi_rate_limit = 10000\n' "$2" "$sock" > "$1"
  chmod 600 "$1"
}
ready() { python3 - "$sock" <<'PY'
import socket, sys, time
deadline = time.time() + 30
while time.time() < deadline:
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
        s.sendall(b"GET /v1/health/ready HTTP/1.1\r\nHost: l\r\n\r\n")
        if s.recv(64).startswith(b"HTTP/1.1 200"): sys.exit(0)
    except OSError:
        pass
    time.sleep(0.01)
sys.exit(1)
PY
}
now_ms() { python3 -c 'import time; print(int(time.monotonic()*1000))'; }

conf "$work/off.conf" false
echo "{\"idle_no_api\":$("$agent" --config "$work/off.conf" --duration "$idle" --report 2>/dev/null)}"
conf "$work/on.conf" true
echo "{\"idle_api\":$("$agent" --config "$work/on.conf" --duration "$idle" --report 2>/dev/null)}"

# Startup and load: one agent serves the benchmark, then reports its peak RSS.
t0=$(now_ms)
"$agent" --config "$work/on.conf" --duration 600 --report > "$work/load.json" 2>/dev/null &
apid=$!
ready || { echo "agent not ready"; kill $apid; exit 1; }
echo "{\"startup\":{\"ms_to_ready\":$(( $(now_ms) - t0 ))}}"
"$bench" "$sock" --requests 200 --clients 16 --seconds 10 --burst 200
# Shutdown with 16 clients active.
"$bench" "$sock" --requests 1 --clients 16 --seconds 30 --burst 1 >/dev/null 2>&1 &
bpid=$!
sleep 3
t1=$(now_ms); kill -TERM $apid; wait $apid || true; t2=$(now_ms)
kill $bpid 2>/dev/null || true; wait $bpid 2>/dev/null || true
echo "{\"under_load\":$(cat "$work/load.json")}"
echo "{\"shutdown_under_load\":{\"ms\":$(( t2 - t1 ))}}"
