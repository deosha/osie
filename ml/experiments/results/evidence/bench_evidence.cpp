// SPDX-License-Identifier: Apache-2.0
// Evidence-quality benchmark (ml/experiments/results/evidence/README.md).
// Builds against libosie_core of any v0.3 tree (it uses only the API both
// main and Feature 4 share), so the same source measures before and after.
//
//   bench_evidence [processes=2000] [repeats=300]
//
// A mock Linux machine with N processes (CPU times and memory consistent
// with real elapsed time, PSI, quota-limited cgroups) feeds a real
// AgentRuntime; DiagnosticApi answers in-process. Prints JSON lines:
//   diagnose    per question: engine_ms and handle_ms p50/p99, response bytes
//   history     approx_bytes and RSS growth with a full history (default
//               capacity 720, top_k 16)
//   temporal    temporal questions and /v1/system/history with that history
//   refusals    RefusingOs refusals over the whole run (must be 0)
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "osie/api/api.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::api;
using Clock = std::chrono::steady_clock;

namespace {

double rss_mb() {
    std::ifstream f("/proc/self/statm");
    long pages = 0, res = 0;
    if (f >> pages >> res) return static_cast<double>(res) * static_cast<double>(sysconf(_SC_PAGESIZE)) / 1048576.0;
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_maxrss) / 1048576.0;  // macOS: bytes (peak)
}

struct Stats {
    double p50, p99, max;
};
Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
    return {at(0.5), at(0.99), v.back()};
}

HttpResponse ask(DiagnosticApi& api, const std::string& q) {
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/diagnose";
    r.content_type = "application/json";
    r.body = "{\"question\":\"" + q + "\",\"session_id\":\"bench\"}";
    r.content_length = r.body.size();
    PeerCredentials p;
    p.known = true;
    return api.handle(r, p, Access::full);
}

}  // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 2000;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 300;
    const auto t0 = Clock::now();
    auto os = std::make_unique<test::MockOs>(Platform::linux_os);
    os->cpu_gen = [t0](int) {
        const auto t = static_cast<std::uint64_t>(std::chrono::duration<double>(Clock::now() - t0).count() * 1e4);
        return test::MockOs::cpu(t * 6 / 10 * 4, t * 4 / 10 * 4);
    };
    MemoryMetrics m;
    m.total_bytes = 64ULL << 30;
    m.available_bytes = 20ULL << 30;
    os->mem_script = {m};
    os->proc_gen = [t0, n](int) {
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        ProcessSnapshot snap;
        snap.processes.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double share = i < 4 ? 0.8 / (i + 1) : 0.0005 * (i % 7);
            auto p = test::proc(100 + i, 1, i == 0 ? "nginx" : "proc" + std::to_string(i % 300),
                                static_cast<std::uint64_t>(s * share * 1e9), (16ULL << 20) + static_cast<std::uint64_t>(i % 97) * (3ULL << 20),
                                1000, static_cast<Uid>(1000 + i % 5));
            p.path = "/usr/bin/p" + std::to_string(i % 300);
            if (i < 8) p.cgroup = "/system.slice/svc" + std::to_string(i) + ".service";
            snap.processes.push_back(std::move(p));
        }
        return snap;
    };
    PressureStall c;
    c.resource = PsiResource::cpu;
    c.some.avg10 = 12;
    PressureStall mm;
    mm.resource = PsiResource::memory;
    mm.some.avg10 = 2;
    os->psi = {c, mm};
    os->cgroup_gen = [t0](const std::string& path, int) {
        CgroupInfo g;
        g.path = path;
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        g.cpu_quota_us = 50000;
        g.cpu_period_us = 100000;
        g.nr_periods = 100000 + static_cast<std::uint64_t>(s * 10);
        g.nr_throttled = 30000 + static_cast<std::uint64_t>(s * 4);
        g.usage_usec = 4000000 + static_cast<std::uint64_t>(s * 5e5);
        g.throttled_usec = 1000000 + static_cast<std::uint64_t>(s * 2e5);
        return g;
    };
    agent::AgentConfig cfg;  // defaults: history_capacity 720, history_top_k 16
    cfg.sample_window = Millis{20};
    auto rt = std::make_shared<agent::AgentRuntime>(std::move(os), cfg);
    ApiSettings st;
    st.max_evidence_age = Millis{3'600'000};
    DiagnosticApi api(rt, st);

    const double rss0 = rss_mb();
    rt->collect_now();
    rt->collect_now();
    const char* qs[] = {"what is using the most CPU?", "why is my CPU usage high?", "why is my machine slow?",
                        "which process uses the most memory?", "is memory pressure high?", "why is nginx using so much CPU?",
                        "tell me about pid 101", "is a container being CPU-throttled?"};
    std::vector<double> all_engine;
    for (const char* q : qs) {
        std::vector<double> eng, tot;
        std::size_t bytes = 0;
        int status = 0;
        for (int i = 0; i < repeats; ++i) {
            const auto a = Clock::now();
            const auto r = ask(api, q);
            tot.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
            eng.push_back(r.engine_ms);
            all_engine.push_back(r.engine_ms);
            bytes = r.body.size();
            status = r.status;
        }
        const auto e = stats(eng), t = stats(tot);
        std::printf("{\"diagnose\":{\"question\":\"%s\",\"status\":%d,\"bytes\":%zu,\"engine_p50_ms\":%.4f,\"engine_p99_ms\":%.4f,"
                    "\"handle_p50_ms\":%.4f,\"handle_p99_ms\":%.4f,\"n\":%d}}\n",
                    q, status, bytes, e.p50, e.p99, t.p50, t.p99, repeats);
    }
    const auto ea = stats(all_engine);
    std::printf("{\"diagnose_all\":{\"processes\":%d,\"engine_p50_ms\":%.4f,\"engine_p99_ms\":%.4f,\"n\":%zu}}\n", n, ea.p50, ea.p99,
                all_engine.size());

    // Fill the history to its default capacity.
    const double rss1 = rss_mb();
    const auto f0 = Clock::now();
    while (rt->health().history_points < cfg.history_capacity) rt->collect_now();
    const double fill_s = std::chrono::duration<double>(Clock::now() - f0).count();
    const auto h = rt->health();
    std::printf("{\"history\":{\"points\":%zu,\"top_k\":%zu,\"approx_bytes\":%zu,\"rss_before_fill_mb\":%.2f,\"rss_after_fill_mb\":%.2f,"
                "\"rss_at_start_mb\":%.2f,\"fill_s\":%.1f}}\n",
                h.history_points, cfg.history_top_k, h.history_bytes, rss1, rss_mb(), rss0, fill_s);

    for (const char* q : {"which process is growing in memory?", "has cpu usage increased in the last 10 minutes?",
                          "is memory pressure increasing?", "is pid 100 using more memory over time?"}) {
        std::vector<double> eng, tot;
        std::size_t bytes = 0;
        int status = 0;
        for (int i = 0; i < std::max(20, repeats / 5); ++i) {
            const auto a = Clock::now();
            const auto r = ask(api, q);
            tot.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
            eng.push_back(r.engine_ms);
            bytes = r.body.size();
            status = r.status;
        }
        const auto e = stats(eng), t = stats(tot);
        std::printf("{\"temporal\":{\"question\":\"%s\",\"status\":%d,\"bytes\":%zu,\"engine_p50_ms\":%.4f,\"engine_p99_ms\":%.4f,"
                    "\"handle_p50_ms\":%.4f,\"handle_p99_ms\":%.4f}}\n",
                    q, status, bytes, e.p50, e.p99, t.p50, t.p99);
    }
    {
        std::vector<double> tot;
        std::size_t bytes = 0;
        for (int i = 0; i < 100; ++i) {
            HttpRequest r;
            r.method = "GET";
            r.path = "/v1/system/history";
            r.query = "window_seconds=86400&limit=1000";
            PeerCredentials p;
            p.known = true;
            const auto a = Clock::now();
            const auto resp = api.handle(r, p, Access::full);
            tot.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
            bytes = resp.body.size();
        }
        const auto t = stats(tot);
        std::printf("{\"history_endpoint\":{\"points\":%zu,\"bytes\":%zu,\"p50_ms\":%.4f,\"p99_ms\":%.4f}}\n", h.history_points, bytes,
                    t.p50, t.p99);
    }
    std::printf("{\"refusals\":{\"refused_calls\":%llu}}\n", static_cast<unsigned long long>(api.os().refused_calls()));
}
