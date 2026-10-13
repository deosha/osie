// SPDX-License-Identifier: Apache-2.0
// osie_perf: per-stage latency budget (docs/BENCHMARKS.md, "v0.2 budget").
//
//   osie_perf [--utterances FILE] [--world-model PATH] [--iterations N] [--watch-seconds S]
//
// Reports p50/p99 in microseconds for: rule parsing, the full understanding
// pipeline, each native observation, reasoning on a sampled state, world-model
// loading and inference, and end-to-end ask() latency split into the sampling
// window (sleep) and computation. Also peak RSS and the CPU time of repeated
// background sampling. Prints one JSON object.
#include <sys/resource.h>
#include <unistd.h>

#include <thread>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "osie/architecture/model.hpp"
#include "osie/engine/engine.hpp"
#include "osie/prediction/world_model.hpp"
#include "osie/util/json.hpp"

using namespace osie;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats {
    double p50{0}, p99{0}, mean{0}, max{0};
    std::size_t n{0};
};

Stats stats(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.n = v.size();
    s.p50 = v[v.size() / 2];
    s.p99 = v[std::min(v.size() - 1, static_cast<std::size_t>(0.99 * static_cast<double>(v.size())))];
    s.max = v.back();
    for (double x : v) s.mean += x;
    s.mean /= static_cast<double>(v.size());
    return s;
}

double us_since(Clock::time_point t0) { return std::chrono::duration<double, std::micro>(Clock::now() - t0).count(); }

Stats time_n(std::size_t n, const std::function<void()>& f) {
    std::vector<double> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        f();
        v.push_back(us_since(t0));
    }
    return stats(std::move(v));
}

double cpu_seconds() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
           static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

double peak_rss_mb() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
#if defined(__APPLE__)
    return static_cast<double>(r.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(r.ru_maxrss) / 1024.0;
#endif
}

void put(json::Writer& w, const std::string& k, const Stats& s) {
    w.key(k).begin_object().kv("p50_us", s.p50).kv("p99_us", s.p99).kv("mean_us", s.mean).kv("max_us", s.max).kv("n", s.n).end_object();
}

}  // namespace

int main(int argc, char** argv) {
    std::string utt_path, wm_path;
    std::size_t iters = 2000;
    long watch_s = 10;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--utterances") utt_path = next();
        else if (a == "--world-model") wm_path = next();
        else if (a == "--iterations") iters = std::stoul(next());
        else if (a == "--watch-seconds") watch_s = std::stol(next());
    }
    std::vector<std::string> utts = {"what processes are running?", "why is my machine slow", "kill process 123",
                                     "how much memory is postgres using", "how does kill -9 work?"};
    if (!utt_path.empty()) {
        utts.clear();
        std::ifstream in(utt_path);
        for (std::string l; std::getline(in, l);)
            if (!l.empty()) utts.push_back(l);
    }

    json::Writer w;
    w.begin_object().kv("platform", to_string(host_platform())).kv("utterances", utts.size());

    // ---- language -----------------------------------------------------------
    intent::RuleParser rules;
    const auto reg = CapabilityRegistry::builtin();
    auto os = make_native_os();
    const auto sup = os->supported();
    std::vector<double> parse_us, pipeline_us;
    for (std::size_t r = 0; r < std::max<std::size_t>(1, iters / utts.size()); ++r) {
        for (const auto& u : utts) {
            auto t0 = Clock::now();
            auto toks = intent::tokenize(u);
            (void)rules.parse(toks);
            parse_us.push_back(us_since(t0));
            t0 = Clock::now();
            (void)intent::understand(u, rules, os->platform(), sup, reg);
            pipeline_us.push_back(us_since(t0));
        }
    }
    put(w, "rule_parse", stats(parse_us));
    put(w, "understanding_pipeline", stats(pipeline_us));

    // ---- native observations --------------------------------------------------
    SafetyPolicy pol;
    AuditLog audit;
    ObservationBroker broker(*os, reg, pol, audit);
    w.key("observations").begin_object();
    const std::size_t on = 200;
    put(w, "system_info", time_n(on, [&] { (void)broker.system_info(); }));
    put(w, "cpu_topology", time_n(on, [&] { (void)broker.cpu_topology(); }));
    put(w, "cpu_sample", time_n(on, [&] { (void)broker.cpu_sample(); }));
    put(w, "memory", time_n(on, [&] { (void)broker.memory(); }));
    put(w, "process_list", time_n(50, [&] { (void)broker.processes(ProcessQuery{}, {}); }));
    put(w, "filesystems", time_n(on, [&] { (void)broker.filesystems(); }));
    put(w, "network_interfaces", time_n(on, [&] { (void)broker.network_interfaces(); }));
    put(w, "cpu_capacity", time_n(on, [&] { (void)broker.cpu_capacity(0); }));
    if (os->linux_ext() != nullptr) {
        put(w, "psi", time_n(on, [&] { (void)broker.pressure(); }));
        std::string cg = "/";
        if (auto c = broker.cgroup_of(0 + static_cast<Pid>(::getpid()))) cg = *c;
        put(w, "cgroup", time_n(on, [&] { (void)broker.cgroup(cg); }));
    }
    w.end_object();

    // ---- reasoning on one sampled state ---------------------------------------
    {
        StateSampler sampler(broker);
        auto st = sampler.sample(SampleRequest{os->supported().list(), Millis{200}});
        arch::KnowledgeGraph g;
        arch::load_ontology(g);
        arch::update_instances(g, st, os->platform());
        reason::Reasoner rs(g, os->platform());
        w.key("reasoning").begin_object();
        put(w, "diagnose_cpu", time_n(500, [&] { (void)rs.cpu(st); }));
        put(w, "diagnose_memory", time_n(500, [&] { (void)rs.memory(st); }));
        put(w, "diagnose_slow", time_n(500, [&] { (void)rs.slowness(st); }));
        put(w, "graph_update", time_n(100, [&] { (void)arch::update_instances(g, st, os->platform()); }));
        w.end_object();
    }

    // ---- world model -----------------------------------------------------------
    if (!wm_path.empty()) {
        std::shared_ptr<predict::WorldModel> m;
        const auto load = time_n(200, [&] {
            auto r = predict::WorldModel::load(wm_path);
            if (r) m = *r;
        });
        put(w, "world_model_load", load);
        if (m) {
            predict::CgroupWindow cw;
            cw.secs = 0.5;
            cw.usage_cores = 1.4;
            cw.throttled_ratio = 0.5;
            cw.throttled_frac = 0.8;
            cw.psi_some = 0.2;
            cw.psi_full = 0.1;
            cw.quota_cores = 1.5;
            cw.cpuset_cpus = 4;
            cw.pids = 6;
            cw.host_busy_frac = 0.3;
            cw.host_cpus = 12;
            const std::vector<predict::CgroupWindow> h(m->history_windows(), cw);
            const predict::Intervention a{predict::ActionKind::cpu_quota, 1.5, 1.0};
            put(w, "world_model_predict", time_n(20000, [&] { (void)m->predict(h, a); }));
            w.kv("world_model_parameters", m->parameter_count());
        }
    }

    // ---- end to end -------------------------------------------------------------
    {
        EngineConfig cfg;
        cfg.freshness_budget = Millis{0};  // no cache reuse: every ask samples
        Engine e(make_native_os(), std::make_unique<intent::RuleParser>(), cfg);
        std::vector<double> total_ms, compute_ms;
        const double window_ms = static_cast<double>(cfg.sample_interval.count());
        for (int r = 0; r < 3; ++r) {
            for (const auto& u : {std::string("what is using the most cpu"), std::string("why is my machine slow"),
                                  std::string("how much memory is free"), std::string("what processes are running")}) {
                const auto t0 = Clock::now();
                (void)e.ask(u);
                const double ms = us_since(t0) / 1000.0;
                total_ms.push_back(ms);
                compute_ms.push_back(std::max(0.0, ms - window_ms));
            }
        }
        auto t = stats(total_ms), c = stats(compute_ms);
        w.key("ask_end_to_end").begin_object().kv("sampling_window_ms", window_ms)
            .kv("total_p50_ms", t.p50).kv("total_max_ms", t.max)
            .kv("excluding_window_p50_ms", c.p50).kv("excluding_window_max_ms", c.max).kv("n", t.n).end_object();

        // Repeated background sampling (the opt-in --watch sampler).
        const double c0 = cpu_seconds();
        const auto t0 = Clock::now();
        for (long i = 0; i < watch_s; ++i) {
            const auto s0 = Clock::now();
            e.record_sample();
            std::this_thread::sleep_until(s0 + std::chrono::seconds(1));
        }
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        w.key("watch_sampling").begin_object().kv("period_s", 1).kv("seconds", wall)
            .kv("cpu_share_of_one_core", (cpu_seconds() - c0) / wall)
            .kv("history_bytes", e.history_bytes()).end_object();
    }
    w.kv("peak_rss_mb", peak_rss_mb());
    w.end_object();
    std::puts(w.str().c_str());
    return 0;
}
