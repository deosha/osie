#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Measurements of `osie remote` against a real sshd and agent, run by
# tests/platform/remote_ssh_test.sh --bench OUT (as root; the commands run as
# the given user). Writes one JSON object per line to OUT/remote_bench.jsonl.
# Answers (exit 0), API refusals (exit 1, by error code) and transport
# failures (exit 3, by remote_error code) are counted separately: a refused
# request is never counted as a success.
import json, os, socket, statistics, subprocess, sys, threading, time

osie, user, home, sock, out_dir = sys.argv[1:6]
N = int(os.environ.get("BENCH_N", "50"))
SECONDS = float(os.environ.get("BENCH_SECONDS", "10"))
Q = "what is using the most CPU?"
pw = __import__("pwd").getpwnam(user)
ENV = {"HOME": home, "PATH": "/usr/bin:/bin", "USER": user}
lines = []


def emit(d):
    d["host"] = {"nproc": os.cpu_count(), "kernel": os.uname().release, "machine": os.uname().machine}
    lines.append(d)
    print(json.dumps(d), flush=True)


def demote():
    os.setgroups(os.getgrouplist(user, pw.pw_gid))
    os.setgid(pw.pw_gid)
    os.setuid(pw.pw_uid)


def run(args, timeout=60):
    """Runs osie as the user; returns (exit, stdout, wall_ms, rusage)."""
    t0 = time.perf_counter()
    p = subprocess.Popen([osie, "remote", *args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=ENV,
                         preexec_fn=demote)
    out = p.stdout.read()
    _, status, ru = os.wait4(p.pid, 0)
    ms = (time.perf_counter() - t0) * 1000
    p.returncode = os.waitstatus_to_exitcode(status)
    return p.returncode, out, ms, ru


STDERR_SAMPLES = {}


def classify(rc, out):
    try:
        d = json.loads(out)
    except Exception:
        return "exit%d:unparsable" % rc
    if rc == 3:  # keep what ssh said, a few distinct lines per code
        e = d["remote_error"]
        seen = STDERR_SAMPLES.setdefault(e["code"], [])
        line = (e.get("ssh_stderr") or e.get("message", "")).splitlines()[:1]
        if line and line[0] not in seen and len(seen) < 3:
            seen.append(line[0])
    if rc == 0:
        return "answered_2xx"
    if rc == 1:
        return "api_error:" + d.get("error", {}).get("code", d.get("status", "?"))
    if rc == 3:
        return "transport:" + d["remote_error"]["code"]
    return "exit%d" % rc


def pct(v, p):
    v = sorted(v)
    return round(v[min(len(v) - 1, int(p * len(v)))], 2) if v else None


def summary(v):
    return {"n": len(v), "p50_ms": pct(v, 0.5), "p90_ms": pct(v, 0.9), "p99_ms": pct(v, 0.99),
            "max_ms": round(max(v), 2) if v else None, "mean_ms": round(statistics.mean(v), 2) if v else None}


def local_ask():
    """One diagnose on the agent socket directly, in this process, as the user (the local baseline)."""
    body = json.dumps({"question": Q}).encode()
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.sendall(b"POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s"
              % (len(body), body))
    d = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        d += b
    s.close()
    return int(d[9:12])


# 1. SSH establishment (spawn -> forwarding socket ready) and request latency inside one connection.
connect, ready, caps, total = [], [], [], []
for _ in range(N):
    rc, out, ms, _ = run(["status", "local", "--json"])
    if rc != 0:
        continue
    d = json.loads(out)
    connect.append(d["ssh_connect_ms"]); ready.append(d["ready"]["latency_ms"])
    caps.append(d["capabilities"]["latency_ms"]); total.append(ms)
emit({"bench": "ssh_establishment", "what": "osie remote status: spawn of ssh until the forwarding socket exists",
      "ok": len(connect), "runs": N, "ssh_connect": summary(connect), "request_ready": summary(ready),
      "request_capabilities": summary(caps), "command_wall": summary(total)})

# 2. End-to-end remote ask (whole osie process) vs the local socket (one in-process request).
remote, outcomes, rss_kb, cpu_ms = [], {}, [], []
for _ in range(N):
    rc, out, ms, ru = run(["ask", "local", Q, "--json"])
    c = classify(rc, out)
    outcomes[c] = outcomes.get(c, 0) + 1
    if c == "answered_2xx":
        remote.append(ms)
        rss_kb.append(ru.ru_maxrss)
        cpu_ms.append((ru.ru_utime + ru.ru_stime) * 1000)
emit({"bench": "remote_ask_end_to_end", "what": "wall time of `osie remote ask --json`, process start to exit",
      "outcomes": outcomes, "answered_latency": summary(remote)})
emit({"bench": "client_resources", "what": "wait4() rusage of osie, which includes the ssh it spawned and reaped",
      "max_rss_kb": {"p50": pct(rss_kb, 0.5), "max": max(rss_kb) if rss_kb else None},
      "cpu_ms_user_plus_sys": {"p50": pct(cpu_ms, 0.5), "max": round(max(cpu_ms), 2) if cpu_ms else None}})

local, lstat = [], {}
pid = os.fork()
if pid == 0:  # measure as the same user
    demote()
    for _ in range(N):
        t0 = time.perf_counter()
        st = local_ask()
        lstat[st] = lstat.get(st, 0) + 1
        if st == 200:
            local.append((time.perf_counter() - t0) * 1000)
    with open(os.path.join(out_dir, ".local.json"), "w") as f:
        json.dump({"local": local, "statuses": lstat}, f)
    os._exit(0)
os.waitpid(pid, 0)
with open(os.path.join(out_dir, ".local.json")) as f:
    lo = json.load(f)
os.unlink(os.path.join(out_dir, ".local.json"))
emit({"bench": "local_socket_ask", "what": "one POST /v1/diagnose on the agent socket, in-process client, same user",
      "statuses": lo["statuses"], "answered_latency": summary(lo["local"])})

# 3. Concurrent remote clients: each a loop of `osie remote ask` for SECONDS.
for clients in (1, 4, 16):
    lock = threading.Lock()
    lat, outc = [], {}
    stop = time.monotonic() + SECONDS

    def worker():
        while time.monotonic() < stop:
            rc, out, ms, _ = run(["ask", "local", Q, "--json"])
            c = classify(rc, out)
            with lock:
                outc[c] = outc.get(c, 0) + 1
                if c == "answered_2xx":
                    lat.append(ms)

    t0 = time.monotonic()
    ts = [threading.Thread(target=worker) for _ in range(clients)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    wall = time.monotonic() - t0
    emit({"bench": "concurrent_remote_ask", "clients": clients, "seconds": round(wall, 2), "outcomes": outc,
          "answered_per_s": round(len(lat) / wall, 2), "answered_latency": summary(lat),
          "transport_stderr_samples": dict(STDERR_SAMPLES)})
    STDERR_SAMPLES.clear()

# 4. Timeouts and cleanup: a server that accepts TCP and never speaks.
for t in (1, 2, 5):
    v = []
    for _ in range(3):
        rc, out, ms, _ = run(["status", "blackhole", "--json", "--timeout", str(t)])
        v.append(ms if classify(rc, out) == "transport:timeout" else float("nan"))
    left = subprocess.run(["pgrep", "-u", user, "-x", "ssh"], capture_output=True).stdout.strip()
    dirs = [x for x in os.listdir("/tmp") if x.startswith("osie-remote-")]
    emit({"bench": "timeout", "timeout_s": t, "elapsed_ms": [round(x, 1) for x in v],
          "overshoot_ms_max": round(max(v) - t * 1000, 1), "ssh_left": len(left.split()) if left else 0,
          "private_dirs_left": len(dirs)})

# 5. Signal to cleanup: SIGTERM to osie while ssh waits for the banner.
v = []
for _ in range(5):
    p = subprocess.Popen([osie, "remote", "status", "blackhole", "--json", "--timeout", "30"], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, env=ENV, preexec_fn=demote)
    time.sleep(0.5)
    t0 = time.perf_counter()
    p.terminate()
    p.wait()
    v.append((time.perf_counter() - t0) * 1000)
left = subprocess.run(["pgrep", "-u", user, "-x", "ssh"], capture_output=True).stdout.strip()
emit({"bench": "sigterm_cleanup", "what": "SIGTERM to osie until it has stopped ssh, removed its directory and exited",
      "ms": summary(v), "ssh_left": len(left.split()) if left else 0,
      "private_dirs_left": len([x for x in os.listdir("/tmp") if x.startswith("osie-remote-")])})

with open(os.path.join(out_dir, "remote_bench.jsonl"), "a") as f:
    for d in lines:
        f.write(json.dumps(d) + "\n")
