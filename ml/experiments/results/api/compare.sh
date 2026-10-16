#!/bin/bash
# Feature 1 vs Feature 2 on the same host: idle cost, and OS access under API load.
set -u
command -v strace >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq strace >/dev/null 2>&1; }
mkdir -p /opt/f1 /opt/f2
dpkg-deb -x /s/pkg_base/osie_0.2.0_arm64.deb /opt/f1
dpkg-deb -x /s/pkg6/osie_0.2.0_arm64.deb /opt/f2
id bench >/dev/null 2>&1 || useradd -m bench
cp /s/osie_api_bench /tmp/b && chmod 755 /tmp/b
su bench -s /bin/bash -c 'ONLY_TRACE=${ONLY_TRACE:-0}; 
set -u
cd /tmp
w=$(mktemp -d /tmp/cmp.XXXX); chmod 700 $w
for i in $(seq 1 2000); do sleep 100000 & done
echo "{\"processes_on_host\":$(ls /proc | grep -c "^[0-9]"),\"nproc\":$(nproc),\"kernel\":\"$(uname -r)\"}"
printf "interval = 5s\nsample_window = 500ms\n" > $w/off.conf
printf "interval = 5s\nsample_window = 500ms\napi_enabled = true\napi_socket = %s/a.sock\napi_rate_limit = 10000\napi_max_clients_per_user = 16\n" $w > $w/on.conf
chmod 600 $w/*.conf
idle() {  # idle <name> <agent> <conf>
  $2 --config $3 --duration 120 --report --measure-freshness > $w/r.json 2>/dev/null &
  local p=$!; sleep 60
  local th=$(awk "/^Threads/{print \$2}" /proc/$p/status) fds=$(ls /proc/$p/fd | wc -l)
  wait $p
  echo "{\"variant\":\"$1\",\"threads\":$th,\"fds\":$fds,\"report\":$(cat $w/r.json)}"
}
if [ "${ONLY_TRACE:-0}" != 1 ]; then
idle feature1 /opt/f1/usr/bin/osie-agent $w/off.conf
idle feature2_api_off /opt/f2/usr/bin/osie-agent $w/off.conf
idle feature2_api_on /opt/f2/usr/bin/osie-agent $w/on.conf
fi
# OS access: /proc opens per collection, idle and under 100 clients.
trace() {  # trace <name> <load: 0|1>
  rm -f $w/a.sock
  strace -f -qq -e trace=openat -o $w/t.txt /opt/f2/usr/bin/osie-agent --config $w/on.conf --duration 40 --report --measure-freshness > $w/r.json 2>/dev/null &
  local p=$!
  for i in $(seq 1 200); do [ -S $w/a.sock ] && break; sleep 0.05; done
  if [ "$2" = 1 ]; then /tmp/b $w/a.sock --sweep 100 --seconds 35 > $w/load.json 2>/dev/null; fi
  wait $p
  local opens=$(grep -c "openat(.*\"/proc" $w/t.txt)
  local coll=$(python3 -c "import json; print(json.load(open(\"$w/r.json\"))[\"health\"][\"collections\"])")
  local age=$(python3 -c "import json; d=json.load(open(\"$w/r.json\")); print(json.dumps(d.get(\"snapshot_age_ms\")))")
  local served=$( [ "$2" = 1 ] && python3 -c "import json; d=json.load(open(\"$w/load.json\")); print(d[\"answered\"])" || echo 0)
  echo "{\"trace\":\"$1\",\"collections\":$coll,\"proc_opens\":$opens,\"proc_opens_per_collection\":$(python3 -c "print(round($opens/max(1,$coll)))"),\"api_answers\":$served,\"snapshot_age_ms\":$age}"
}
trace idle 0
trace load_100_clients 1
'
