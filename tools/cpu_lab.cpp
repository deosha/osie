// SPDX-License-Identifier: Apache-2.0
// osie_cpu_lab (Linux only): controlled CPU-contention experiments.
//
//   osie_cpu_lab probe --duration 8 --window-ms 500 --period-us 10000 --work 200000 --label X --out F.jsonl
//       Runs a periodic latency probe (fixed work every period) while sampling
//       native telemetry with OSIE's Linux adapter. Each window emits one JSON
//       line with features (what OSIE can observe) and the probe's latency
//       outcome (what a user would feel).
//   osie_cpu_lab load --mode spin|duty|hash|io --threads N [--duty 0.5] --duration S
//       Background load generator.
//
// Intended to run inside disposable, resource-limited containers only.
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "osie/platform/os.hpp"
#include "osie/util/json.hpp"

using namespace osie;
using clk = std::chrono::steady_clock;

namespace {

std::string arg(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

std::uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

volatile std::uint64_t g_sink = 0;

void fixed_work(std::uint64_t iters) {
    std::uint64_t x = 88172645463325252ULL;
    for (std::uint64_t i = 0; i < iters; ++i) {  // xorshift: ALU-bound, no memory traffic
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
    }
    g_sink = g_sink + x;
}

void hash_work(std::vector<std::uint32_t>& buf) {  // memory + ALU mix (different instruction profile)
    std::uint32_t h = 2166136261U;
    for (auto& v : buf) {
        h = (h ^ v) * 16777619U;
        v = h;
    }
    g_sink = g_sink + h;
}

int run_load(int argc, char** argv) {
    const std::string mode = arg(argc, argv, "--mode", "spin");
    const int threads = std::stoi(arg(argc, argv, "--threads", "1"));
    const double duty = std::stod(arg(argc, argv, "--duty", "1.0"));
    const double duration = std::stod(arg(argc, argv, "--duration", "10"));
    const auto end = clk::now() + std::chrono::duration<double>(duration);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            std::vector<std::uint32_t> buf(1 << 18, static_cast<std::uint32_t>(t));
            std::string path = "/tmp/osie_io_" + std::to_string(::getpid()) + "_" + std::to_string(t);
            std::vector<char> block(1 << 16, 'x');
            while (clk::now() < end) {
                if (mode == "spin") {
                    fixed_work(100000);
                } else if (mode == "hash") {
                    hash_work(buf);
                } else if (mode == "duty") {  // busy for duty*10ms, sleep the rest
                    const auto b = clk::now();
                    while (clk::now() - b < std::chrono::microseconds(static_cast<long>(duty * 10000))) fixed_work(2000);
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>((1.0 - duty) * 10000)));
                } else if (mode == "io") {
                    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                    if (fd >= 0) {
                        for (int i = 0; i < 16; ++i)
                            if (::write(fd, block.data(), block.size()) < 0) break;
                        ::fsync(fd);
                        ::close(fd);
                    }
                }
            }
            ::unlink(path.c_str());
        });
    }
    for (auto& t : ts) t.join();
    return 0;
}

struct Snap {
    std::optional<CpuSample> cpu;
    std::vector<PressureStall> psi;
    std::optional<CgroupInfo> cg;
    std::uint64_t t_ns{0};
};

double psi_rate(const std::vector<PressureStall>& a, const std::vector<PressureStall>& b, PsiResource r, bool full,
                double secs) {
    for (const auto& x : a)
        for (const auto& y : b)
            if (x.resource == r && y.resource == r) {
                if (full) {
                    if (!x.full || !y.full) return 0;
                    return static_cast<double>(y.full->total_us - x.full->total_us) / (secs * 1e6);
                }
                return static_cast<double>(y.some.total_us - x.some.total_us) / (secs * 1e6);
            }
    return -1;
}

int run_probe(int argc, char** argv) {
    const double duration = std::stod(arg(argc, argv, "--duration", "8"));
    const auto window_ms = std::stol(arg(argc, argv, "--window-ms", "500"));
    const auto period_us = std::stol(arg(argc, argv, "--period-us", "10000"));
    const auto work = std::stoull(arg(argc, argv, "--work", "200000"));
    const std::string label = arg(argc, argv, "--label", "unknown");
    const std::string run_id = arg(argc, argv, "--run", "r");
    const std::string out_path = arg(argc, argv, "--out", "/dev/stdout");
    const int warmup = std::stoi(arg(argc, argv, "--warmup-windows", "2"));

    auto os = make_native_os();
    auto* lx = os->linux_ext();
    std::string cg_path = "/";
    if (lx) {
        if (auto r = lx->cgroup_of(::getpid())) cg_path = *r;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    const int cpuset = CPU_COUNT(&set);

    std::mutex mu;
    std::vector<double> lat_us;  // completion latency relative to the period deadline
    std::atomic<bool> stop{false};
    std::thread probe([&] {
        std::uint64_t deadline = now_ns();
        while (!stop.load(std::memory_order_relaxed)) {
            deadline += static_cast<std::uint64_t>(period_us) * 1000ULL;
            timespec ts{static_cast<time_t>(deadline / 1'000'000'000ULL), static_cast<long>(deadline % 1'000'000'000ULL)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            fixed_work(work);
            const double l = static_cast<double>(now_ns() - deadline) / 1000.0;
            {
                std::lock_guard lk(mu);
                lat_us.push_back(l);
            }
            const std::uint64_t n = now_ns();
            if (n > deadline + static_cast<std::uint64_t>(period_us) * 1000ULL) deadline = n;  // skip missed periods
        }
    });

    auto take = [&]() {
        Snap s;
        s.t_ns = now_ns();
        if (auto c = os->cpu_sample()) s.cpu = c->value;
        if (lx) {
            if (auto p = lx->pressure()) s.psi = p->value;
            if (auto c = lx->cgroup(cg_path)) s.cg = c->value;
        }
        return s;
    };

    std::ofstream out(out_path);
    Snap prev = take();
    const auto end = clk::now() + std::chrono::duration<double>(duration);
    int w = 0;
    while (clk::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
        Snap cur = take();
        std::vector<double> lat;
        {
            std::lock_guard lk(mu);
            lat.swap(lat_us);
        }
        const double secs = static_cast<double>(cur.t_ns - prev.t_ns) / 1e9;
        if (w++ >= warmup && !lat.empty() && cur.cpu && prev.cpu) {
            std::sort(lat.begin(), lat.end());
            auto q = [&](double p) { return lat[std::min(lat.size() - 1, static_cast<std::size_t>(p * static_cast<double>(lat.size())))]; };
            const auto& a = prev.cpu->aggregate;
            const auto& b = cur.cpu->aggregate;
            const double dt = static_cast<double>(b.total() - a.total());
            json::Writer j;
            j.begin_object().kv("run", run_id).kv("label", label).kv("window", w).kv("cpuset", cpuset);
            // ---- features (observable by OSIE) ----
            j.kv("host_busy", dt > 0 ? 100.0 * static_cast<double>(b.busy() - a.busy()) / dt : 0.0)
                .kv("host_user", dt > 0 ? 100.0 * static_cast<double>(b.user - a.user) / dt : 0.0)
                .kv("host_system", dt > 0 ? 100.0 * static_cast<double>(b.system - a.system) / dt : 0.0)
                .kv("host_iowait", dt > 0 ? 100.0 * static_cast<double>(b.iowait - a.iowait) / dt : 0.0)
                .kv("host_steal", dt > 0 ? 100.0 * static_cast<double>(b.steal - a.steal) / dt : 0.0)
                .kv("runnable", cur.cpu->runnable_tasks.value_or(0))
                .kv("blocked", cur.cpu->blocked_tasks.value_or(0))
                .kv("load1", cur.cpu->load ? cur.cpu->load->one : 0.0)
                .kv("ctxt_per_s", cur.cpu->context_switches_total && prev.cpu->context_switches_total
                                      ? static_cast<double>(*cur.cpu->context_switches_total - *prev.cpu->context_switches_total) / secs
                                      : 0.0)
                .kv("psi_cpu_some", psi_rate(prev.psi, cur.psi, PsiResource::cpu, false, secs))
                .kv("psi_mem_some", psi_rate(prev.psi, cur.psi, PsiResource::memory, false, secs))
                .kv("psi_io_some", psi_rate(prev.psi, cur.psi, PsiResource::io, false, secs))
                .kv("psi_io_full", psi_rate(prev.psi, cur.psi, PsiResource::io, true, secs));
            if (cur.cg && prev.cg) {
                const auto& x = *prev.cg;
                const auto& y = *cur.cg;
                auto d = [](std::optional<std::uint64_t> p, std::optional<std::uint64_t> q) {
                    return p && q && *q >= *p ? static_cast<double>(*q - *p) : 0.0;
                };
                const double periods = d(x.nr_periods, y.nr_periods);
                j.kv("cg_usage_cores", d(x.usage_usec, y.usage_usec) / (secs * 1e6))
                    .kv("cg_throttled_ratio", periods > 0 ? d(x.nr_throttled, y.nr_throttled) / periods : 0.0)
                    .kv("cg_throttled_frac", d(x.throttled_usec, y.throttled_usec) / (secs * 1e6))
                    .kv("cg_quota_cores", y.cpu_quota_us && y.cpu_period_us
                                              ? static_cast<double>(*y.cpu_quota_us) / static_cast<double>(*y.cpu_period_us)
                                              : 0.0);
                double cg_psi = 0;
                for (const auto& p : x.pressure)
                    for (const auto& r : y.pressure)
                        if (p.resource == PsiResource::cpu && r.resource == PsiResource::cpu)
                            cg_psi = static_cast<double>(r.some.total_us - p.some.total_us) / (secs * 1e6);
                j.kv("cg_psi_cpu_some", cg_psi);
            }
            // ---- outcome (what the user feels) ----
            j.kv("lat_p50_us", q(0.5)).kv("lat_p95_us", q(0.95)).kv("lat_p99_us", q(0.99)).kv("lat_max_us", lat.back())
                .kv("completions", lat.size());
            j.end_object();
            out << j.str() << '\n';
        }
        prev = cur;
    }
    stop = true;
    probe.join();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: osie_cpu_lab probe|load ...\n");
        return 2;
    }
    if (std::strcmp(argv[1], "load") == 0) return run_load(argc, argv);
    if (std::strcmp(argv[1], "probe") == 0) return run_probe(argc, argv);
    return 2;
}
