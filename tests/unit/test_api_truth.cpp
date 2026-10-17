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
    JsonValue ask(const std::string& question, uid_t uid = 0, Access a = Access::full, const std::string& session = "t",
                  const std::string& extra = "") {
        HttpRequest r;
        r.method = "POST";
        r.path = "/v1/diagnose";
        r.content_type = "application/json";
        r.body = "{\"question\":\"" + question + "\",\"session_id\":\"" + session + "\"" + extra + "}";
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

/// Provenance of every evidence item: an id unique in the response, the
/// snapshot the answer used, a reading taken before that snapshot was
/// published, and, for derived values other than cgroup lifetime ratios, the
/// two readings they are the difference of.
void expect_provenance(const JsonValue& v) {
    if (!v.has("evidence")) return;
    std::set<std::string> ids;
    for (const auto& e : v["evidence"].arr()) {
        const std::string sig = e["signal"].str();
        ASSERT_TRUE(e.has("evidence_id")) << sig;
        EXPECT_TRUE(ids.insert(e["evidence_id"].str()).second) << "duplicate id " << e["evidence_id"].str();
        EXPECT_EQ(e["evidence_id"].str().find('/'), std::string::npos) << "ids carry no paths";
        ASSERT_TRUE(v.has("observation")) << sig;
        EXPECT_EQ(e["snapshot_seq"].num(), v["observation"]["snapshot_seq"].num()) << sig;
        if (e.has("sampled_at") && e.has("observed_at"))
            EXPECT_LE(e["sampled_at"].str(), e["observed_at"].str()) << sig << ": read after the snapshot was published?";
        if (e.has("freshness")) {
            EXPECT_EQ(e["freshness"].str(), "fresh") << sig;
            EXPECT_LE(e["age_ms"].num(), v["observation"]["max_age_ms"].num()) << sig;
        }
        const bool cgroup = e.has("observation") && e["observation"].str() == "cgroup";
        if (e["validity"].str() == "derived" && !cgroup && sig != "cpu_unattributed_cores") {
            ASSERT_TRUE(e.has("window")) << sig << " is derived but has no window";
        }
        if (e.has("window")) {
            EXPECT_GT(e["window"]["seconds"].num(), 0.0) << sig;
            if (e["window"]["start"].is_string() && e["window"]["end"].is_string()) {
                EXPECT_LT(e["window"]["start"].str(), e["window"]["end"].str()) << sig;
                EXPECT_LE(e["window"]["end"].str(), e["observed_at"].str()) << sig;
            }
        }
    }
}

const JsonValue* trend(const JsonValue& v, const std::string& contains) {
    if (!v.has("trends")) return nullptr;
    for (const auto& t : v["trends"].arr())
        if (t["series"].str().find(contains) != std::string::npos) return &t;
    return nullptr;
}

bool has_note(const JsonValue& e, const std::string& note) {
    if (!e.has("quality_notes")) return false;
    for (const auto& n : e["quality_notes"].arr())
        if (n.str() == note) return true;
    return false;
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
    expect_provenance(v);
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
    // Provenance: the host figure is a rate over the two CPU readings of the
    // collection, at least the 100 ms sampling window apart.
    EXPECT_EQ((*busy)["observation"].str(), "cpu_times");
    EXPECT_EQ((*busy)["observation_validity"].str(), "derived");
    EXPECT_EQ((*busy)["source"].str(), "mock.cpu");
    ASSERT_TRUE(busy->has("window"));
    EXPECT_GE((*busy)["window"]["seconds"].num(), 0.1);
    EXPECT_LT((*busy)["window"]["seconds"].num(), 1.0);
    EXPECT_FALSE((*busy)["window"].has("from_snapshot_seq")) << "within one collection";
    ASSERT_TRUE(ng->has("window"));
    EXPECT_EQ((*ng)["observation"].str(), "process_list");
    EXPECT_GE((*ng)["window"]["seconds"].num(), 0.1);
    EXPECT_EQ(v["observation"]["freshness"].str(), "fresh");
    EXPECT_EQ(v["observation"]["snapshot_seq"].num(), static_cast<double>(rig.rt->snapshot()->seq));
    EXPECT_FALSE(v.has("history_window")) << "not a temporal question";
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
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
    // Provenance: a rate across the agent's last two collections, over the
    // interval between the two reads of cpu.stat (one per collection, at the
    // same point of each), with no extra read.
    const auto [cur, prev] = rig.rt->snapshots();
    ASSERT_TRUE(cur && prev);
    EXPECT_EQ((*thr)["observation"].str(), "cgroup");
    EXPECT_EQ((*thr)["validity"].str(), "derived");
    ASSERT_TRUE(thr->has("window")) << rig.last.body.substr(0, 800);
    EXPECT_EQ((*thr)["window"]["from_snapshot_seq"].num(), static_cast<double>(prev->seq));
    EXPECT_EQ(v["observation"]["previous_snapshot_seq"].num(), static_cast<double>(prev->seq));
    const double between = std::chrono::duration<double>(cur->mono - prev->mono).count();
    EXPECT_NEAR((*thr)["window"]["seconds"].num(), between, 0.15) << "collections " << between << " s apart";
    EXPECT_GE((*thr)["window"]["seconds"].num(), 0.3) << "the test slept 300 ms between collections";
    EXPECT_TRUE(has_note(*thr, "between_collections"));
    EXPECT_FALSE(has_note(*thr, "cumulative_not_rate"));
    // interval_s is the interval the rate was computed over, not the
    // 100 ms sampling window of one collection.
    EXPECT_NEAR((*thr)["interval_s"].num(), (*thr)["window"]["seconds"].num(), 0.05);
    for (const auto& e : v["diagnosis"]["evidence"].arr())
        if (e["id"].str() == "cgroup_throttled:/system.slice/nginx.service")
            EXPECT_NEAR(e["interval_s"].num(), (*thr)["window"]["seconds"].num(), 0.05);
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
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
    // The only figure left is the lifetime ratio, and it says so: never a rate.
    if (const auto* thr = evidence(v, "cgroup_throttled:/system.slice/nginx.service")) {
        EXPECT_NE((*thr)["validity"].str(), "derived");
        EXPECT_FALSE(thr->has("window"));
        EXPECT_TRUE(has_note(*thr, "counter_reset")) << rig.last.body.substr(0, 1500);
        EXPECT_TRUE(has_note(*thr, "cumulative_not_rate"));
    }
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
    const auto* thr = evidence(v, "cgroup_throttled:/system.slice/nginx.service");
    ASSERT_NE(thr, nullptr) << rig.last.body.substr(0, 800);
    EXPECT_EQ((*thr)["validity"].str(), "valid");
    EXPECT_FALSE(thr->has("window")) << "no rate across a quota change";
    EXPECT_TRUE(has_note(*thr, "quota_changed"));
    EXPECT_TRUE(has_note(*thr, "cumulative_not_rate"));
    // The lifetime ratio is the scenario's throttled share of periods.
    EXPECT_NEAR((*thr)["value"].num(), 0.4, 0.01);
}

// 5. A process's memory grows steadily: found by the agent's history.
// With it, a process that grew and then exited, and whose PID was then
// reused by a process of constant size: the exited one is reported as not
// running, and the new holder of the PID is never stitched into its series.
TEST(ApiTruth, ProcessMemoryGrowthFromHistory) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 200ULL << 20, 40ULL << 20}, {200, "postgres", kBob, 0.1, 2000ULL << 20}};
    auto c = cfg(Millis{50});
    c.interval = Millis{250};
    Rig rig(sc, c);
    static constexpr std::uint64_t kLeakerStart = 7'000'000, kFreshStart = 9'000'000;
    {
        // pid 300: "leaker" grows 60 MiB/s from 100 MiB for 11 s and exits;
        // then "fresh" (a new process, same PID) holds 80 MiB.
        std::lock_guard lk(rig.mock->mu);
        auto gen = rig.mock->proc_gen;
        const auto t0 = Clock::now();
        rig.mock->proc_gen = [gen, t0](int n) {
            auto snap = gen(n);
            const double t = std::chrono::duration<double>(Clock::now() - t0).count();
            auto p = t < 11.0 ? owned(300, "leaker", kAlice, 0, (100ULL << 20) + static_cast<std::uint64_t>(t * (60 << 20)), "/x")
                              : owned(300, "fresh", kAlice, 0, 80ULL << 20, "/x");
            p.start_time_us = t < 11.0 ? kLeakerStart : kFreshStart;
            snap.processes.push_back(p);
            return snap;
        };
    }
    ASSERT_TRUE(rig.rt->start().ok());
    std::this_thread::sleep_for(Millis{13'000});  // trends need >= 10 s of history
    rig.rt->stop();
    auto v = rig.ask("which process is growing in memory?");
    expect_well_founded(rig, v);
    // "fresh" has held pid 300 for about 2 s, too little to judge: the
    // ranking is incomplete, and says so.
    EXPECT_EQ(v["status"].str(), "partial") << rig.last.body.substr(0, 1500);
    EXPECT_TRUE(has_lim(v, "insufficient_history", "too little history of their own"));
    ASSERT_TRUE(v.has("trends")) << rig.last.body.substr(0, 1500);
    bool nginx_up = false, postgres_up = false;
    for (const auto& t : v["trends"].arr()) {
        if (t["series"].str().find("nginx") != std::string::npos) nginx_up = t["direction"].str() == "increasing";
        if (t["series"].str().find("postgres") != std::string::npos) postgres_up = t["direction"].str() == "increasing";
    }
    const auto* fr = trend(v, "fresh");
    ASSERT_NE(fr, nullptr) << rig.last.body.substr(0, 1500);
    EXPECT_EQ((*fr)["direction"].str(), "insufficient_data") << "not judged, not called growth";
    EXPECT_EQ((*fr)["start_time_us"].num(), static_cast<double>(kFreshStart));
    EXPECT_TRUE((*fr)["running"].boolean());
    EXPECT_NE(v["answer"]["text"].str().find("Too new to judge"), std::string::npos);
    EXPECT_TRUE(nginx_up) << rig.last.body.substr(0, 900);
    EXPECT_FALSE(postgres_up) << "postgres is constant";
    // Identity and liveness of each growing series.
    const auto* ng = trend(v, "nginx");
    ASSERT_NE(ng, nullptr);
    EXPECT_EQ((*ng)["pid"].num(), 100);
    EXPECT_TRUE((*ng)["running"].boolean());
    const auto* lk = trend(v, "leaker");
    ASSERT_NE(lk, nullptr) << rig.last.body.substr(0, 1500);
    EXPECT_EQ((*lk)["direction"].str(), "increasing");
    EXPECT_EQ((*lk)["pid"].num(), 300);
    EXPECT_EQ((*lk)["start_time_us"].num(), static_cast<double>(kLeakerStart));
    EXPECT_FALSE((*lk)["running"].boolean()) << "it exited; pid 300 now belongs to another process";
    EXPECT_LT((*lk)["last_point_at"].str(), (*ng)["last_point_at"].str()) << "its series ends when it exited";
    // The window: everything the agent recorded, continuously.
    ASSERT_TRUE(v.has("history_window"));
    const auto& hw = v["history_window"];
    EXPECT_TRUE(hw["requested_s"].is_null());
    EXPECT_EQ(hw["points"].num(), static_cast<double>(rig.rt->history_copy().size()));
    EXPECT_GE(hw["observed_span_s"].num(), 10.0);
    EXPECT_GE(hw["coverage"].num(), kMinHistoryCoverage) << rig.last.body.substr(0, 2000);
    EXPECT_TRUE(hw["sufficient"].boolean());
    EXPECT_FALSE(has_lim(v, "history_gap"));
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
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
    for (const auto& e : v["evidence"].arr())
        if (e.has("observation")) EXPECT_NE(e["observation"].str(), "pressure_stall") << "no PSI provenance invented";
    const auto* avail = evidence(v, "mem_available_share");
    ASSERT_NE(avail, nullptr);
    EXPECT_EQ((*avail)["observation"].str(), "memory");
    EXPECT_EQ((*avail)["observation_validity"].str(), "valid");
    EXPECT_FALSE(avail->has("window")) << "an instantaneous reading";
    EXPECT_NEAR((*avail)["value"].num(), 300.0 / (16 * 1024), 1e-4);
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

// ---------------------------------------------------------------------------
// Freshness, historical windows and abstention (v0.3 Feature 4).

// A stale snapshot is refused; an answer within the limit states the age of
// its snapshot and of each reading, all within the limit.
TEST(ApiTruth, StaleSnapshotAndPerItemFreshness) {
    Scenario sc;
    sc.busy = 0.5;
    sc.procs = {{100, "nginx", kAlice, 1.0, 300ULL << 20}};
    Rig rig(sc);
    rig.collect();
    std::this_thread::sleep_for(Millis{300});
    auto stale = rig.ask("why is my CPU usage high?", 0, Access::full, "t", ",\"max_evidence_age_ms\":100");
    EXPECT_EQ(rig.last.status, 503);
    EXPECT_EQ(stale["error"]["code"].str(), "stale_evidence");
    auto v = rig.ask("why is my CPU usage high?", 0, Access::full, "t", ",\"max_evidence_age_ms\":60000");
    expect_well_founded(rig, v);
    ASSERT_EQ(rig.last.status, 200);
    const auto& ob = v["observation"];
    EXPECT_GE(ob["age_ms"].num(), 300.0) << "the snapshot is at least as old as the pause";
    EXPECT_EQ(ob["max_age_ms"].num(), 60000);
    EXPECT_EQ(ob["freshness"].str(), "fresh");
    ASSERT_FALSE(v["evidence"].arr().empty());
    for (const auto& e : v["evidence"].arr()) {
        if (!e.has("age_ms")) continue;
        // A reading is taken before its snapshot is published, so it is at
        // least as old as the snapshot.
        EXPECT_GE(e["age_ms"].num() + 1.0, ob["age_ms"].num()) << e["signal"].str();
        EXPECT_EQ(e["freshness"].str(), "fresh");
    }
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
}

// The agent stopped collecting for 1.5 s inside the window asked about.
TEST(ApiTruth, HistoryGapIsReportedNotInterpolated) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 300ULL << 20}};
    auto c = cfg(Millis{20});  // gap threshold: 2 x 250 ms + 20 ms = 0.52 s
    Rig rig(sc, c);
    rig.collect(6, Millis{100});
    const auto pause = Millis{1500};
    std::this_thread::sleep_for(pause);
    rig.collect(6, Millis{100});
    auto v = rig.ask("has cpu usage increased in the last 10 seconds?");
    expect_well_founded(rig, v);
    ASSERT_TRUE(v.has("history_window")) << rig.last.body.substr(0, 1500);
    const auto& hw = v["history_window"];
    EXPECT_EQ(hw["requested_s"].num(), 10);
    EXPECT_EQ(hw["points"].num(), 12);
    EXPECT_EQ(hw["first_snapshot_seq"].num(), 1);
    EXPECT_EQ(hw["last_snapshot_seq"].num(), 12);
    EXPECT_NEAR(hw["gap_threshold_s"].num(), 0.52, 1e-9);
    int inner = 0;
    double gap_s = 0;
    for (const auto& g : hw["gaps"].arr())
        if (g["kind"].str() == "collection_gap") {
            ++inner;
            gap_s = g["seconds"].num();
        }
    EXPECT_EQ(inner, 1) << rig.last.body.substr(0, 2000);
    // Between the last point before the pause and the first after it: the
    // pause plus one collection.
    EXPECT_GE(gap_s, 1.5);
    EXPECT_LT(gap_s, 1.5 + 1.0);
    // 12 points over ~2.5 s of a 10 s window: far from sufficient.
    EXPECT_LT(hw["coverage"].num(), kMinHistoryCoverage);
    EXPECT_FALSE(hw["sufficient"].boolean());
    EXPECT_EQ(v["status"].str(), "partial");
    EXPECT_TRUE(has_lim(v, "history_gap", "nothing is interpolated"));
    EXPECT_TRUE(has_lim(v, "insufficient_history", "of the requested 10 s"));
    // The history endpoint lists the same gap, and ties points to snapshots.
    HttpRequest r;
    r.method = "GET";
    r.path = "/v1/system/history";
    r.query = "window_seconds=60";
    PeerCredentials p;
    p.known = true;
    const auto h = JsonReader::parse(rig.api->handle(r, p, Access::full).body);
    ASSERT_EQ(h["gaps"].arr().size(), 1U) << h["gaps"].arr().size();
    EXPECT_EQ(h["gaps"][0]["kind"].str(), "collection_gap");
    EXPECT_NEAR(h["gaps"][0]["seconds"].num(), gap_s, 1e-6);
    ASSERT_EQ(h["points"].arr().size(), 12U);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(h["points"][i]["snapshot_seq"].num(), static_cast<double>(i + 1));
    EXPECT_EQ(h["coverage"]["points"].num(), 12);
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
}

// "In the last 10 minutes" when the agent has recorded a fraction of a second.
TEST(ApiTruth, WindowLongerThanHistoryHeld) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 300ULL << 20}};
    Rig rig(sc);
    rig.collect(3, Millis{100});
    auto v = rig.ask("has cpu usage increased in the last 10 minutes?");
    expect_well_founded(rig, v);
    ASSERT_TRUE(v.has("history_window")) << rig.last.body.substr(0, 1500);
    const auto& hw = v["history_window"];
    EXPECT_EQ(hw["requested_s"].num(), 600);
    EXPECT_EQ(hw["points"].num(), 3);
    EXPECT_LT(hw["observed_span_s"].num(), 5.0);
    ASSERT_FALSE(hw["gaps"].arr().empty());
    EXPECT_EQ(hw["gaps"][0]["kind"].str(), "before_first_point");
    // Expected from the parameters: at most a few seconds of 600 s observed.
    EXPECT_GT(hw["gaps"][0]["seconds"].num(), 590.0);
    EXPECT_LT(hw["coverage"].num(), 0.02);
    EXPECT_EQ(v["status"].str(), "partial");
    EXPECT_TRUE(has_lim(v, "insufficient_history", "does not reach back that far")) << rig.last.body.substr(0, 2000);
    for (const auto& t : v["trends"].arr()) EXPECT_EQ(t["direction"].str(), "insufficient_data");
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
}

// A crash an hour ago that OSIE never observed: no event is reconstructed.
TEST(ApiTruth, EventNeverObservedIsNotInvented) {
    Scenario sc;
    sc.procs = {{100, "nginx", kAlice, 0.2, 300ULL << 20}};
    Rig rig(sc);
    rig.collect();
    auto v = rig.ask("why did nginx crash an hour ago?");
    expect_well_founded(rig, v);
    EXPECT_EQ(v["status"].str(), "partial") << rig.last.body.substr(0, 800);
    EXPECT_TRUE(has_lim(v, "no_event_history", "past event"));
    const std::string text = v["answer"]["text"].str();
    for (const char* claim : {"crashed", "was killed", "OOM", "segfault"})
        EXPECT_EQ(text.find(claim), std::string::npos) << "no crash is asserted: " << text;
    for (const auto& f : v["diagnosis"]["findings"].arr())
        EXPECT_EQ(f["text"].str().find("crash"), std::string::npos) << f["text"].str();
    // Present-tense questions are not flagged.
    auto now = rig.ask("why is nginx using so much CPU?");
    EXPECT_FALSE(has_lim(now, "no_event_history"));
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
}

// Application latency is not observed: CPU figures can neither show nor
// explain it, and the answer says so instead of implying it.
TEST(ApiTruth, ApplicationLatencyIsNotInferredFromCpu) {
    Scenario sc;
    sc.busy = 0.95;
    sc.procs = {{100, "nginx", kAlice, 3.6, 300ULL << 20}};
    Rig rig(sc);
    rig.collect();
    for (const char* q : {"is high CPU causing my application latency?", "why is nginx latency high?",
                          "why are requests to my app slow?"}) {
        auto v = rig.ask(q);
        expect_well_founded(rig, v);
        EXPECT_EQ(v["status"].str(), "partial") << q;
        EXPECT_TRUE(has_lim(v, "not_observed", "application latency")) << q << ": " << rig.last.body.substr(0, 600);
        if (v.has("diagnosis") && !v["diagnosis"].is_null())
            for (const auto& f : v["diagnosis"]["findings"].arr()) {
                EXPECT_EQ(f["text"].str().find("application latency"), std::string::npos) << q << ": " << f["text"].str();
                EXPECT_EQ(f["text"].str().find("request"), std::string::npos) << q << ": " << f["text"].str();
            }
    }
    // Questions about what OSIE observes are not flagged, nor is the OS's own
    // scheduling latency.
    for (const char* q : {"what is using the most CPU?", "why is my CPU usage high?", "what is scheduling latency?"}) {
        auto v = rig.ask(q);
        EXPECT_FALSE(has_lim(v, "not_observed")) << q;
    }
    EXPECT_EQ(rig.api->os().refused_calls(), 0U);
}

// Partial reads: an incomplete process list is flagged on the evidence
// that came from it, and the collector's own validity is passed through.
TEST(ApiTruth, PartialProcessListIsFlaggedOnItsEvidence) {
    Scenario sc;
    sc.busy = 0.6;
    sc.procs = {{100, "nginx", kAlice, 2.0, 300ULL << 20}};
    Rig rig(sc);
    {
        std::lock_guard lk(rig.mock->mu);
        auto gen = rig.mock->proc_gen;
        rig.mock->proc_gen = [gen](int n) {
            auto snap = gen(n);
            snap.partial_count = 7;  // seven processes could not be read
            snap.vanished_count = 2;
            return snap;
        };
    }
    rig.collect();
    auto v = rig.ask("what is using the most CPU?");
    expect_well_founded(rig, v);
    const auto* ng = evidence(v, "proc_cpu:100");
    ASSERT_NE(ng, nullptr) << rig.last.body.substr(0, 800);
    EXPECT_TRUE(has_note(*ng, "process_list_incomplete"));
    EXPECT_TRUE(has_lim(v, "permission_limited", "7 processes"));
    const auto* busy = evidence(v, "cpu_busy_pct");
    ASSERT_NE(busy, nullptr);
    EXPECT_FALSE(has_note(*busy, "process_list_incomplete")) << "the host figure does not come from the process list";
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
