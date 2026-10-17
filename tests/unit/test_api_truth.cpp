// SPDX-License-Identifier: Apache-2.0
// Truthfulness of API diagnoses against an independent ground truth.
//
// Each scenario is built from physical parameters (the host's busy share,
// each process's share of a CPU, a cgroup's quota and throttled share),
// and the mock derives counters from real elapsed time, so every rate the
// agent computes is consistent. Assertions compare whole responses against
// those parameters: the mechanism reported, evidence values, timestamps,
// scope, certainty, limitations and abstention. A finding must cite only
// evidence that the response actually contains.
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <set>
#include <thread>

#include "osie/api/api.hpp"
#include "support/agent_fixture.hpp"
#include "support/json_value.hpp"

using namespace osie;
using namespace osie::api;
using namespace osie::test;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t elapsed_ns() {
    static const auto t0 = Clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

struct Proc {
    Pid pid;
    std::string name;
    Uid uid;
    double cpus;  // share of one CPU, e.g. 0.5
    std::uint64_t mem;
    std::uint64_t mem_growth_per_s{0};
    std::string cgroup;
};

struct Cgroup {
    std::string path;
    double quota_cpus{0.5};
    double throttled_share{0};  // of periods
    double usage_cpus{0.5};
    // Set by the test between two collections (not by time, so a slow,
    // sanitized collection cannot fall on the wrong side of the change).
    std::shared_ptr<std::atomic<bool>> reset = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> quota_changed = std::make_shared<std::atomic<bool>>(false);
};

/// A consistent machine: 4 logical CPUs, the host `busy` share busy.
struct Scenario {
    double busy{0.3};
    std::vector<Proc> procs;
    std::vector<Cgroup> cgroups;
    bool psi{true};
    std::uint64_t mem_total{16ULL << 30}, mem_available{8ULL << 30};
    std::optional<Errc> cgroup_error;

    std::unique_ptr<MockOs> build() const {
        auto os = std::make_unique<MockOs>(Platform::linux_os);
        const double busy_share = busy;
        os->cpu_gen = [busy_share](int i) {
            const auto t = static_cast<double>(elapsed_ns()) / 1e5;  // 0.1 ms ticks: fine enough for short windows
            (void)i;
            return MockOs::cpu(static_cast<std::uint64_t>(t * busy_share), static_cast<std::uint64_t>(t * (1 - busy_share)));
        };
        MemoryMetrics m;
        m.total_bytes = mem_total;
        m.available_bytes = mem_available;
        os->mem_script = {m};
        const auto ps = procs;
        os->proc_gen = [ps](int) {
            const double s = static_cast<double>(elapsed_ns()) / 1e9;
            ProcessSnapshot snap;
            for (const auto& p : ps) {
                auto pi = owned(p.pid, p.name.c_str(), p.uid, static_cast<std::uint64_t>(s * p.cpus * 1e9),
                                p.mem + static_cast<std::uint64_t>(s * static_cast<double>(p.mem_growth_per_s)), "/usr/bin/x");
                if (!p.cgroup.empty()) pi.cgroup = p.cgroup;
                snap.processes.push_back(pi);
            }
            return snap;
        };
        if (psi) {
            PressureStall c;
            c.resource = PsiResource::cpu;
            c.some.avg10 = busy_share > 0.9 ? 30 : 1;
            os->psi = {c};
        } else {
            ObservationSet sup;
            for (auto k : {ObservationKind::system_info, ObservationKind::cpu_topology, ObservationKind::cpu_times,
                           ObservationKind::load_average, ObservationKind::process_list, ObservationKind::memory,
                           ObservationKind::swap, ObservationKind::cgroup})
                sup.add(k);
            os->set_supported(sup);
        }
        const auto cgs = cgroups;
        const auto err = cgroup_error;
        os->cgroup_gen = [cgs, err](const std::string& path, int) {
            CgroupInfo c;
            c.path = path;
            if (err) return c;  // unreachable: cgroup_ok_calls makes the mock fail first
            for (const auto& g : cgs) {
                if (g.path != path) continue;
                double s = static_cast<double>(elapsed_ns()) / 1e9;
                // A recreated cgroup's counters restart from zero; before that they
                // carry a long history (offset), so a reset always goes backwards.
                std::uint64_t base = 100'000;
                if (*g.reset) {
                    s = std::fmod(s, 0.05);  // recreated moments ago
                    base = 0;
                }
                const double period_s = 0.001;  // fine-grained periods: exact ratios over short windows
                const auto periods = static_cast<std::uint64_t>(s / period_s) + base;
                c.cpu_period_us = 100000;
                const bool changed = *g.quota_changed;
                c.cpu_quota_us = static_cast<std::int64_t>((changed ? g.quota_cpus * 2 : g.quota_cpus) * 100000);
                c.nr_periods = periods;
                c.nr_throttled = static_cast<std::uint64_t>(static_cast<double>(periods) * g.throttled_share);
                c.usage_usec = static_cast<std::uint64_t>(s * g.usage_cpus * 1e6) + base * 100;
                c.throttled_usec = static_cast<std::uint64_t>(s * g.throttled_share * 1e5) + base * 10;
            }
            return c;
        };
        if (err) os->cgroup_ok_calls = 0;
        return os;
    }
};

agent::AgentConfig cfg(Millis window = Millis{100}) {
    agent::AgentConfig c;
    c.interval = Millis{250};
    c.sample_window = window;
    c.history_capacity = 200;
    c.history_top_k = 8;
    c.cgroup_top_n = 8;
    return c;
}

struct Rig {
    explicit Rig(const Scenario& sc, agent::AgentConfig c = cfg(), ProcVisibility vis = ProcVisibility::all) {
        auto os = sc.build();
        mock = os.get();
        rt = std::make_shared<agent::AgentRuntime>(std::move(os), c);
        ApiSettings s;
        s.max_evidence_age = Millis{600'000};
        s.visibility = vis;
        api = std::make_unique<DiagnosticApi>(rt, s);
    }
    void collect(int n = 2, Millis gap = Millis{120}) {
        for (int i = 0; i < n; ++i) {
            ASSERT_TRUE(rt->collect_now());
            if (i + 1 < n) std::this_thread::sleep_for(gap);
        }
    }
    JsonValue ask(const std::string& question, uid_t uid = 0, Access a = Access::full, const std::string& session = "t") {
        HttpRequest r;
        r.method = "POST";
        r.path = "/v1/diagnose";
        r.content_type = "application/json";
        r.body = "{\"question\":\"" + question + "\",\"session_id\":\"" + session + "\"}";
        r.content_length = r.body.size();
        PeerCredentials p;
        p.known = true;
        p.uid = uid;
        last = api->handle(r, p, a);
        return JsonReader::parse(last.body);
    }
    MockOs* mock;
    std::shared_ptr<agent::AgentRuntime> rt;
    std::unique_ptr<DiagnosticApi> api;
    HttpResponse last;
};

bool has_lim(const JsonValue& v, const std::string& code, const std::string& contains = "") {
    if (!v.has("limitations")) return false;
    for (const auto& l : v["limitations"].arr())
        if (l["code"].str() == code && l["detail"].str().find(contains) != std::string::npos) return true;
    return false;
}

const JsonValue* evidence(const JsonValue& v, const std::string& signal) {
    if (!v.has("evidence")) return nullptr;
    for (const auto& e : v["evidence"].arr())
        if (e["signal"].str() == signal) return &e;
    return nullptr;
}

/// Structural truth checks shared by every answered diagnosis.
void expect_well_founded(const Rig& rig, const JsonValue& v) {
    ASSERT_TRUE(v.has("status")) << rig.last.body;
    EXPECT_TRUE(v["answer"]["confidence"].is_null()) << "no uncalibrated confidence";
    EXPECT_FALSE(v["action_executed"].boolean());
    if (!v.has("diagnosis") || v["diagnosis"].is_null()) return;
    std::set<std::string> ids;
    for (const auto& e : v["diagnosis"]["evidence"].arr()) ids.insert(e["id"].str());
    for (const auto& f : v["diagnosis"]["findings"].arr()) {
        for (const auto& id : f["evidence"].arr())
            EXPECT_TRUE(ids.count(id.str())) << "finding '" << f["text"].str() << "' cites missing evidence " << id.str();
        // Only observations can be "high" without a mechanism; hypotheses never are.
        if (f["kind"].str() == "hypothesis") EXPECT_NE(f["certainty"].str(), "high") << f["text"].str();
    }
    // Evidence timestamps are the collection time of the snapshot used.
    const auto collected = iso8601(rig.rt->snapshot()->wall);
    for (const auto& e : v["evidence"].arr())
        if (e.has("observed_at")) EXPECT_EQ(e["observed_at"].str(), collected);
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

}  // namespace

// 1. CPU saturation: the host is 95% busy and nginx takes 3.6 of 4 CPUs.
TEST(ApiTruth, CpuSaturation) {
    Scenario sc;
    sc.busy = 0.95;
    sc.procs = {{100, "nginx", kAlice, 3.6, 300ULL << 20}, {200, "postgres", kBob, 0.1, 2000ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("why is my CPU usage high?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["status"].str(), "completed") << rig.last.body.substr(0, 400);
    EXPECT_NE(v["answer"]["assessment"].str().find("saturated"), std::string::npos) << v["answer"]["assessment"].str();
    const auto* busy = evidence(v, "cpu_busy_pct");
    ASSERT_NE(busy, nullptr);
    EXPECT_TRUE(near((*busy)["value"].num(), 95.0, 1.0)) << (*busy)["value"].num();
    EXPECT_EQ((*busy)["scope"].str(), "host");
    const auto* ng = evidence(v, "proc_cpu:100");
    ASSERT_NE(ng, nullptr);
    EXPECT_TRUE(near((*ng)["value"].num(), 360.0, 40.0)) << (*ng)["value"].num();
    EXPECT_EQ(v["diagnosis"]["subject_pid"].num(), 100);
    EXPECT_FALSE(has_lim(v, "inconsistent_evidence"));
}

// 2. Host CPU available, container throttled by its quota.
TEST(ApiTruth, IdleHostWithAThrottledContainer) {
    Scenario sc;
    sc.busy = 0.15;
    sc.procs = {{100, "nginx", kAlice, 0.5, 300ULL << 20, 0, "/system.slice/nginx.service"}};
    sc.cgroups = {{"/system.slice/nginx.service", 0.5, 0.4, 0.5}};
    Rig rig(sc);
    rig.collect(2, Millis{300});
    auto v = rig.ask("is a container being CPU-throttled?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["answer"]["assessment"].str(), "cpu_quota_bound") << rig.last.body.substr(0, 500);
    const auto* thr = evidence(v, "cgroup_throttled:/system.slice/nginx.service");
    ASSERT_NE(thr, nullptr);
    EXPECT_TRUE(near((*thr)["value"].num(), 0.4, 0.06)) << (*thr)["value"].num();
    const std::string text = v["answer"]["text"].str();
    EXPECT_EQ(text.find("saturated"), std::string::npos) << "the host is not saturated: " << text;
    EXPECT_TRUE(has_lim(v, "cgroup_rates", "between the agent's last two collections"));
    EXPECT_FALSE(has_lim(v, "cgroup_rates", " 0.0 s ")) << "the interval between collections is reported";
}

// 3. Throttling counters reset (cgroup recreated) between collections.
TEST(ApiTruth, ThrottlingCountersResetIsNotARate) {
    Scenario sc;
    sc.busy = 0.15;
    sc.procs = {{100, "nginx", kAlice, 0.5, 300ULL << 20, 0, "/system.slice/nginx.service"}};
    Cgroup g{"/system.slice/nginx.service", 0.5, 0.4, 0.5};
    sc.cgroups = {g};
    Rig rig(sc);
    rig.collect(1);
    std::this_thread::sleep_for(Millis{100});
    *g.reset = true;
    rig.collect(1);
    auto v = rig.ask("is a container being CPU-throttled?");
    expect_well_founded(rig, v);
    EXPECT_TRUE(has_lim(v, "cgroup_rates", "backwards")) << rig.last.body.substr(0, 800);
    EXPECT_EQ(evidence(v, "cgroup_throttled_window:/system.slice/nginx.service"), nullptr);
    EXPECT_NE(v["answer"]["assessment"].str(), "cpu_quota_bound") << "no current-rate conclusion from a reset counter";
}

// 4. The container's CPU quota changes between collections.
TEST(ApiTruth, QuotaChangeIsNotARate) {
    Scenario sc;
    sc.busy = 0.15;
    sc.procs = {{100, "nginx", kAlice, 0.5, 300ULL << 20, 0, "/system.slice/nginx.service"}};
    Cgroup g{"/system.slice/nginx.service", 0.5, 0.4, 0.5};
    sc.cgroups = {g};
    Rig rig(sc);
    rig.collect(1);
    std::this_thread::sleep_for(Millis{100});
    *g.quota_changed = true;
    rig.collect(1);
    auto v = rig.ask("is a container being CPU-throttled?");
    expect_well_founded(rig, v);
    EXPECT_TRUE(has_lim(v, "cgroup_rates", "quota")) << rig.last.body.substr(0, 800);
    EXPECT_NE(v["answer"]["assessment"].str(), "cpu_quota_bound");
}

// 5. A process's memory grows steadily: found by the agent's history.
TEST(ApiTruth, ProcessMemoryGrowthFromHistory) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 200ULL << 20, 40ULL << 20}, {200, "postgres", kBob, 0.1, 2000ULL << 20}};
    auto c = cfg(Millis{50});
    c.interval = Millis{250};
    Rig rig(sc, c);
    ASSERT_TRUE(rig.rt->start().ok());
    std::this_thread::sleep_for(Millis{11'500});  // trends need >= 10 s of history
    rig.rt->stop();
    auto v = rig.ask("which process is growing in memory?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["status"].str(), "completed") << rig.last.body.substr(0, 1500);
    ASSERT_TRUE(v.has("trends")) << rig.last.body.substr(0, 1500);
    bool nginx_up = false, postgres_up = false;
    for (const auto& t : v["trends"].arr()) {
        if (t["series"].str().find("nginx") != std::string::npos) nginx_up = t["direction"].str() == "increasing";
        if (t["series"].str().find("postgres") != std::string::npos) postgres_up = t["direction"].str() == "increasing";
    }
    EXPECT_TRUE(nginx_up) << rig.last.body.substr(0, 900);
    EXPECT_FALSE(postgres_up) << "postgres is constant";
}

// 6 + 10. Memory pressure with incomplete telemetry (no PSI).
TEST(ApiTruth, MemoryPressureWithoutPsiStaysTentative) {
    Scenario sc;
    sc.psi = false;
    sc.mem_available = 300ULL << 20;  // ~2% of 16 GiB
    sc.procs = {{200, "postgres", kBob, 0.1, 14ULL << 30}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("is memory pressure high?");
    expect_well_founded(rig, v);
    EXPECT_NE(v["answer"]["assessment"].str().find("memory_pressure"), std::string::npos) << v["answer"]["assessment"].str();
    EXPECT_NE(v["answer"]["certainty"].str(), "high") << "PSI, the direct stall measurement, is missing";
    EXPECT_TRUE(has_lim(v, "unsupported_on_platform", "pressure_stall"));
    for (const auto& e : v["evidence"].arr()) EXPECT_EQ(e["signal"].str().find("psi"), std::string::npos) << "no PSI evidence invented";
    auto slow = rig.ask("why is my machine slow?");
    expect_well_founded(rig, slow);
    for (const auto& e : slow["evidence"].arr()) EXPECT_EQ(e["signal"].str().find("psi"), std::string::npos);
}

// 7. A process exits between collections.
TEST(ApiTruth, ProcessExitBetweenCollections) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.8, 300ULL << 20}, {200, "postgres", kBob, 0.1, 2000ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto before = rig.ask("why is nginx using so much CPU?");
    EXPECT_EQ(before["diagnosis"]["subject_pid"].num(), 100);
    {
        std::lock_guard lk(rig.mock->mu);
        auto gen = rig.mock->proc_gen;
        rig.mock->proc_gen = [gen](int n) {
            auto s = gen(n);
            s.processes.erase(s.processes.begin());  // nginx exited
            return s;
        };
    }
    rig.collect();
    auto after = rig.ask("why is it using so much CPU?");
    expect_well_founded(rig, after);
    EXPECT_EQ(after["status"].str(), "completed");
    EXPECT_EQ(after["answer"]["assessment"].str(), "process_not_running");
    EXPECT_TRUE(has_lim(after, "no_event_history"));
    EXPECT_FALSE(has_lim(after, "observation_not_collected", "cgroup")) << "no cgroups is not a collection failure";
    EXPECT_FALSE(after.has("diagnosis") && !after["diagnosis"].is_null()) << "no figures for a process that is gone";
}

// 9. Two processes share a name: the busier one is the subject, and the answer says there are two.
TEST(ApiTruth, DuplicateProcessNames) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 300ULL << 20}, {102, "nginx", kAlice, 0.9, 100ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("why is nginx using so much CPU?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["diagnosis"]["subject_pid"].num(), 102) << "the busier instance";
    EXPECT_NE(v["answer"]["text"].str().find("2 processes match"), std::string::npos) << v["answer"]["text"].str();
}

// 11. cgroup information unavailable (permission denied).
TEST(ApiTruth, UnavailableCgroupInformation) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.5, 300ULL << 20, 0, "/system.slice/nginx.service"}};
    sc.cgroups = {{"/system.slice/nginx.service"}};
    sc.cgroup_error = Errc::not_found;
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("is a container being CPU-throttled?");
    expect_well_founded(rig, v);
    for (const auto& e : v["evidence"].arr()) EXPECT_EQ(e["signal"].str().find("cgroup"), std::string::npos) << "no cgroup evidence invented";
    EXPECT_NE(v["answer"]["assessment"].str(), "cpu_quota_bound");
}

// 14. Too little history for a trend.
TEST(ApiTruth, InsufficientHistoryIsNotATrend) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 300ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("has memory usage increased recently?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["status"].str(), "partial");
    EXPECT_TRUE(has_lim(v, "insufficient_history"));
    for (const auto& t : v["trends"].arr()) EXPECT_EQ(t["direction"].str(), "insufficient_data");
}

// 15. A question about an event before the agent started (or any past event).
TEST(ApiTruth, PastEventsAreNotReconstructed) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 300ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("why was nginx killed?");
    expect_well_founded(rig, v);
    EXPECT_TRUE(has_lim(v, "no_event_history")) << rig.last.body.substr(0, 500);
    EXPECT_NE(v["answer"]["text"].str().find("no record of past"), std::string::npos);
}

// 16. Two contradictory observations: processes busier than the whole host.
TEST(ApiTruth, ContradictoryObservationsAreFlagged) {
    Scenario sc;
    sc.busy = 0.02;
    sc.procs = {{100, "nginx", kAlice, 3.0, 300ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("what is using the most CPU?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["status"].str(), "partial");
    EXPECT_TRUE(has_lim(v, "inconsistent_evidence")) << rig.last.body.substr(0, 500);
    // A consistent machine is not flagged.
    Scenario ok;
    ok.busy = 0.8;
    ok.procs = {{100, "nginx", kAlice, 3.0, 300ULL << 20}};
    Rig rig2(ok);
    rig2.collect();
    EXPECT_FALSE(has_lim(rig2.ask("what is using the most CPU?"), "inconsistent_evidence")) << rig2.last.body.substr(0, 400);
}

// 17. Evidence about another user's process, for a restricted peer.
TEST(ApiTruth, RestrictedPeersGetNoRedactedEvidence) {
    Scenario sc;
    sc.procs = {{200, "postgres", kBob, 1.5, 2000ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("why is postgres using so much CPU?", kAlice, Access::diagnostics);
    expect_well_founded(rig, v);
    EXPECT_EQ(rig.last.body.find("/usr/bin/x"), std::string::npos) << "Bob's executable path";
    EXPECT_TRUE(has_lim(v, "redacted"));
}

// 18. A collector fails, then recovers.
TEST(ApiTruth, CollectionFailureAndRecovery) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.8, 300ULL << 20}};
    Rig rig(sc);
    rig.collect(1);
    {
        std::lock_guard lk(rig.mock->mu);
        rig.mock->process_list_error = Errc::io_error;
    }
    rig.collect(1);
    auto failing = rig.ask("what is using the most CPU?");
    expect_well_founded(rig, failing);
    EXPECT_EQ(failing["status"].str(), "partial");
    EXPECT_TRUE(has_lim(failing, "observation_not_collected", "process_list"));
    EXPECT_TRUE(failing["processes"].arr().empty()) << "no per-process figures without a process list";
    {
        std::lock_guard lk(rig.mock->mu);
        rig.mock->process_list_error.reset();
    }
    rig.collect();
    auto ok = rig.ask("what is using the most CPU?");
    EXPECT_EQ(ok["status"].str(), "completed");
    EXPECT_EQ(ok["processes"][0]["pid"].num(), 100);
}

// Unavailable predictions stay unavailable.
TEST(ApiTruth, NoNumericPredictions) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.8, 300ULL << 20, 0, "/system.slice/nginx.service"}};
    sc.cgroups = {{"/system.slice/nginx.service", 1.0, 0.0, 0.8}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("what if I limit nginx to 0.5 cpu?");
    expect_well_founded(rig, v);
    EXPECT_TRUE(has_lim(v, "prediction_not_available"));
    EXPECT_NE(v["answer"]["text"].str().find("can't give a numeric prediction"), std::string::npos);
}

// No duplicate sampling: 100 concurrent diagnoses while the agent samples.
TEST(ApiTruth, ConcurrentDiagnosesNeverObserveTheOs) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.8, 300ULL << 20, 0, "/system.slice/nginx.service"},
                {200, "postgres", kBob, 0.2, 2000ULL << 20}};
    sc.cgroups = {{"/system.slice/nginx.service", 1.0, 0.1, 0.8}};
    auto c = cfg(Millis{20});
    c.interval = Millis{60};
    Rig rig(sc, c);
    ASSERT_TRUE(rig.rt->start().ok());
    while (!rig.rt->snapshot()) std::this_thread::sleep_for(Millis{2});
    const char* qs[] = {"what is using the most CPU?", "why is my machine slow?", "which process uses the most memory?",
                        "is a container being CPU-throttled?", "is memory pressure increasing?", "why is nginx using so much CPU?",
                        "tell me about pid 200", "kill nginx", "what is the weather?", "what is memory compression?"};
    std::atomic<int> answered{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 100; ++i)
        ts.emplace_back([&, i] {
            HttpRequest r;
            r.method = "POST";
            r.path = "/v1/diagnose";
            r.content_type = "application/json";
            r.body = std::string("{\"question\":\"") + qs[i % 10] + "\"}";
            r.content_length = r.body.size();
            PeerCredentials p;
            p.known = true;
            p.uid = i % 2 ? kAlice : 0;
            const auto resp = rig.api->handle(r, p, i % 2 ? Access::diagnostics : Access::full);
            if (resp.status == 200 || resp.status == 422) ++answered;
        });
    for (auto& t : ts) t.join();
    rig.rt->stop();
    EXPECT_EQ(answered.load(), 100);
    // Every OS call came from the agent's own collections: two process lists
    // and two CPU samples per collection (a cancelled one may stop after one).
    const auto h = rig.rt->health();
    int process_calls = 0, cpu_calls = 0;
    {
        std::lock_guard lk(rig.mock->mu);
        process_calls = rig.mock->process_calls;
        cpu_calls = rig.mock->cpu_calls;
    }
    const auto started = static_cast<int>(h.collections) + 1;
    EXPECT_LE(process_calls, 2 * started) << process_calls << " process lists for " << h.collections << " collections";
    EXPECT_LE(cpu_calls, 2 * started) << cpu_calls << " CPU samples for " << h.collections << " collections";
    EXPECT_GE(process_calls, 2 * static_cast<int>(h.collections));
    // And the API's engine never attempted an observation of its own.
    EXPECT_EQ(rig.api->os().refused_calls(), 0U) << "a diagnosis tried to observe the OS";
}

// Under hidepid, a restricted peer sees neither other users' processes nor
// their cgroups: a cgroup path names its owner and service. Without hidepid
// cgroup paths stay visible, as /proc/<pid>/cgroup is world-readable.
TEST(ApiTruth, HiddenUsersCgroupsAreWithheldUnderHidepid) {
    const std::string secret = "/user.slice/user-1001.slice/user@1001.service/app.slice/secretsvc.service";
    const std::string own = "/user.slice/user-1000.slice/user@1000.service/app.slice/web.service";
    Scenario sc;
    sc.busy = 0.3;
    sc.procs = {{100, "web", kAlice, 0.4, 300ULL << 20, 0, own}, {200, "secretsvc", kBob, 0.5, 400ULL << 20, 0, secret}};
    Cgroup hidden{secret, 0.5, 0.4, 0.5};
    sc.cgroups = {{own, 0.5, 0.3, 0.4}, hidden};
    auto get = [](Rig& rig, uid_t uid, Access a, const std::string& path) {
        HttpRequest r;
        r.method = "GET";
        r.path = path;
        PeerCredentials p;
        p.known = true;
        p.uid = uid;
        return rig.api->handle(r, p, a).body;
    };
    {
        Rig rig(sc, cfg(), ProcVisibility::own_only);
        rig.collect(1);
        std::this_thread::sleep_for(Millis{200});
        // The hidden cgroup's quota changes, so a note about it (which would
        // name it) is derived between the two collections.
        *hidden.quota_changed = true;
        rig.collect(1);
        for (const char* q : {"is a container being CPU-throttled?", "which cgroup is constrained?", "why is my machine slow?",
                              "why is my CPU usage high?"}) {
            auto v = rig.ask(q, kAlice, Access::diagnostics);
            expect_well_founded(rig, v);
            for (const char* leak : {"user-1001", "secretsvc", "user@1001"})
                EXPECT_EQ(rig.last.body.find(leak), std::string::npos) << q << " leaks " << leak << ": " << rig.last.body.substr(0, 1500);
        }
        auto own_q = rig.ask("is a container being CPU-throttled?", kAlice, Access::diagnostics);
        EXPECT_NE(rig.last.body.find("web.service"), std::string::npos) << "the peer's own cgroup stays visible";
        const auto snap = get(rig, kAlice, Access::diagnostics, "/v1/system/snapshot");
        EXPECT_EQ(snap.find("secretsvc"), std::string::npos) << snap.substr(0, 1500);
        EXPECT_NE(snap.find("web.service"), std::string::npos);
        // The agent's own (full) view is unchanged.
        const auto full = get(rig, 0, Access::full, "/v1/system/snapshot");
        EXPECT_NE(full.find("secretsvc"), std::string::npos);
        EXPECT_EQ(rig.api->os().refused_calls(), 0U);
    }
    {
        *hidden.quota_changed = false;
        Rig rig(sc);  // no hidepid
        rig.collect(2, Millis{200});
        const auto snap = get(rig, kAlice, Access::diagnostics, "/v1/system/snapshot");
        EXPECT_NE(snap.find("secretsvc"), std::string::npos) << "world-readable without hidepid";
    }
}
