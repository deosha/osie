// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "osie/architecture/model.hpp"
#include "osie/reasoning/reasoner.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::reason;
using osie::test::proc;

namespace {

struct Fixture {
    arch::KnowledgeGraph g;
    Fixture() { arch::load_ontology(g); }
};

SystemState cpu_state(double busy_pct, std::vector<ProcessView> procs, double load1 = 1.0, std::uint32_t ncpu = 4) {
    SystemState s;
    s.interval = Millis{500};
    CpuTopology t;
    t.logical_cpus = ncpu;
    s.topology = Observation<CpuTopology>{t, Provenance::now("t")};
    CpuUtilization u;
    u.user_pct = busy_pct;
    u.idle_pct = 100.0 - busy_pct;
    u.per_cpu_busy_pct.assign(ncpu, busy_pct);
    s.cpu = Observation<CpuUtilization>{u, Provenance::now("t", Validity::derived)};
    s.load = Observation<LoadAverage>{LoadAverage{load1, load1, load1}, Provenance::now("t")};
    s.processes = Observation<std::vector<ProcessView>>{std::move(procs), Provenance::now("t")};
    return s;
}

bool has_finding(const Diagnosis& d, std::string_view code) {
    for (const auto& f : d.findings)
        if (f.code == code) return true;
    return false;
}

const Finding* get_finding(const Diagnosis& d, std::string_view code) {
    for (const auto& f : d.findings)
        if (f.code == code) return &f;
    return nullptr;
}

}  // namespace

TEST(ReasonCpu, SaturatedByOneSingleThreadedProcess) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto s = cpu_state(30.0, {{proc(10, 1, "spinner", 0, 1), 100.0, false}, {proc(11, 1, "idle", 0, 1), 1.0, false}});
    auto d = r.cpu(s);
    EXPECT_EQ(d.conclusion, "cpu_dominated_by_process");
    ASSERT_TRUE(d.subject_pid);
    EXPECT_EQ(*d.subject_pid, 10);
    EXPECT_TRUE(has_finding(d, "single_thread_bound"));
    const auto* f = get_finding(d, "single_thread_bound");
    EXPECT_EQ(f->kind, FindingKind::hypothesis) << "single-thread claim is a hypothesis without per-thread data";
    EXPECT_TRUE(has_finding(d, "reduce_dominant_process"));
    EXPECT_EQ(get_finding(d, "reduce_dominant_process")->kind, FindingKind::proposed_optimization);
    EXPECT_NE(get_finding(d, "reduce_dominant_process")->text.find("Not executed"), std::string::npos);
    EXPECT_FALSE(d.missing_evidence.empty());
}

TEST(ReasonCpu, SaturationWithQueueingCitesMechanisms) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    std::vector<ProcessView> v;
    for (int i = 0; i < 6; ++i) v.push_back({proc(100 + i, 1, "worker" + std::to_string(i), 0, 1), 66.0, false});
    auto s = cpu_state(98.0, v, /*load1=*/7.5);
    auto d = r.cpu(s);
    EXPECT_EQ(d.conclusion, "cpu_saturated");
    const auto* f = get_finding(d, "run_queue_contention");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->kind, FindingKind::causal_explanation);
    ASSERT_GE(f->mechanisms.size(), 2U);
    EXPECT_EQ(f->mechanisms[0].nature, arch::EdgeNature::invariant);
    EXPECT_EQ(f->mechanisms[1].nature, arch::EdgeNature::hypothesized);
}

TEST(ReasonCpu, IdleMachine) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto d = r.cpu(cpu_state(5.0, {{proc(10, 1, "a", 0, 1), 3.0, false}}, 0.3));
    EXPECT_EQ(d.conclusion, "cpu_mostly_idle");
    EXPECT_FALSE(d.subject_pid);
}

TEST(ReasonCpu, UnattributedCpuFromUnreadableProcesses) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto s = cpu_state(90.0, {{proc(10, 1, "small", 0, 1), 20.0, false}}, 4.0);
    s.processes_partial = 50;
    auto d = r.cpu(s);
    EXPECT_TRUE(has_finding(d, "cpu_in_unreadable_processes"));
}

TEST(ReasonCpu, MissingObservationGivesNoConclusion) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    SystemState s;
    s.unavailable.push_back({ObservationKind::cpu_times, make_error(Errc::permission_denied, "x")});
    auto d = r.cpu(s);
    EXPECT_EQ(d.conclusion, "insufficient_evidence");
    EXPECT_EQ(d.certainty, Certainty::none);
    ASSERT_FALSE(d.missing_evidence.empty());
    EXPECT_NE(d.missing_evidence[0].find("permission_denied"), std::string::npos);
}

TEST(ReasonCpu, MacThermalHypothesis) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto s = cpu_state(60.0, {{proc(10, 1, "a", 0, 1), 50.0, false}});
    s.power = Observation<MacPowerState>{MacPowerState{ThermalState::serious, false}, Provenance::now("t")};
    auto d = r.cpu(s);
    const auto* f = get_finding(d, "thermal_throttling_possible");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->kind, FindingKind::hypothesis);
    ASSERT_FALSE(f->mechanisms.empty());
}

TEST(ReasonCpu, LinuxPsiAndCgroupThrottling) {
    Fixture fx;
    Reasoner r(fx.g, Platform::linux_os);
    auto s = cpu_state(70.0, {{proc(10, 1, "app", 0, 1), 50.0, false}});
    PressureStall ps;
    ps.resource = PsiResource::cpu;
    ps.some.avg10 = 35.0;
    s.psi = Observation<std::vector<PressureStall>>{{ps}, Provenance::now("t")};
    CgroupView cv;
    cv.info.path = "/system.slice/app.service";
    cv.info.cpu_quota_us = 50000;
    cv.info.cpu_period_us = 100000;
    cv.lifetime_throttled_ratio = 0.4;
    s.cgroups.push_back(cv);
    auto d = r.cpu(s);
    EXPECT_TRUE(has_finding(d, "cpu_stall_measured"));
    const auto* f = get_finding(d, "cgroup_cpu_throttling");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->kind, FindingKind::causal_explanation);
    ASSERT_FALSE(f->mechanisms.empty());
    EXPECT_EQ(f->mechanisms[0].nature, arch::EdgeNature::invariant);
}

TEST(ReasonMemory, LevelsAndHistoricalSwap) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    SystemState s;
    MemoryMetrics m;
    m.total_bytes = 16ULL << 30;
    m.available_bytes = 8ULL << 30;
    m.free_bytes = 1ULL << 30;
    m.swap_used_bytes = 2ULL << 30;
    m.swap_total_bytes = 4ULL << 30;
    m.pressure_level = MemoryPressureLevel::normal;
    s.memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    MemoryRates rates;
    rates.swapouts_per_s = 0.0;
    s.memory_rates = Observation<MemoryRates>{rates, Provenance::now("t", Validity::derived)};
    auto d = r.memory(s);
    EXPECT_EQ(d.conclusion, "memory_ok");
    EXPECT_TRUE(has_finding(d, "historical_swap")) << "swap usage without swap activity is correlation, not cause";
    EXPECT_EQ(get_finding(d, "historical_swap")->kind, FindingKind::correlation);

    m.pressure_level = MemoryPressureLevel::critical;
    s.memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    d = r.memory(s);
    EXPECT_EQ(d.conclusion, "memory_pressure_high");
    const auto* f = get_finding(d, "memory_pressure");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->kind, FindingKind::causal_explanation);

    m.pressure_level = MemoryPressureLevel::warning;
    s.memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    EXPECT_EQ(r.memory(s).conclusion, "possible_memory_pressure");
}

TEST(ReasonMemory, LinuxPsiFull) {
    Fixture fx;
    Reasoner r(fx.g, Platform::linux_os);
    SystemState s;
    MemoryMetrics m;
    m.total_bytes = 8ULL << 30;
    m.available_bytes = 4ULL << 30;
    s.memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    PressureStall ps;
    ps.resource = PsiResource::memory;
    ps.some.avg10 = 20;
    ps.full = PsiLine{8.0, 1, 1, 1};
    s.psi = Observation<std::vector<PressureStall>>{{ps}, Provenance::now("t")};
    EXPECT_EQ(r.memory(s).conclusion, "memory_pressure_high");
}

TEST(ReasonProcess, NotReadableAndExited) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto other = proc(5, 1, "rootd", 0, 0);
    other.footprint_bytes.reset();
    other.resident_bytes.reset();
    other.validity = Validity::partial;
    auto s = cpu_state(10, {{other, std::nullopt, false}});
    EXPECT_EQ(r.process_memory(s, 5).conclusion, "process_memory_not_readable");
    EXPECT_EQ(r.process_cpu(s, 5).conclusion, "process_cpu_not_readable");
    EXPECT_EQ(r.process_cpu(s, 999).conclusion, "process_not_found");
}

TEST(ReasonProcess, MultiProcessFamily) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto s = cpu_state(10, {{proc(10, 1, "Google Chrome", 0, 1ULL << 30), 5.0, false},
                            {proc(11, 10, "Google Chrome Helper", 0, 2ULL << 30), 5.0, false},
                            {proc(12, 10, "Google Chrome Helper (GPU)", 0, 1ULL << 30), 5.0, false}});
    MemoryMetrics m;
    m.total_bytes = 16ULL << 30;
    s.memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    auto d = r.process_memory(s, 10);
    EXPECT_TRUE(has_finding(d, "multi_process_app"));
    EXPECT_TRUE(has_finding(d, "possible_leak"));
    EXPECT_EQ(get_finding(d, "possible_leak")->certainty, Certainty::low);
}

TEST(ReasonSlowness, CombinesSubsystems) {
    Fixture fx;
    Reasoner r(fx.g, Platform::macos);
    auto s = cpu_state(95.0, {{proc(10, 1, "a", 0, 1), 380.0, false}}, 6.0);
    MemoryMetrics m;
    m.total_bytes = 16ULL << 30;
    m.pressure_level = MemoryPressureLevel::critical;
    s.memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    auto d = r.slowness(s);
    EXPECT_EQ(d.conclusion, "likely_resource_bottleneck");
    EXPECT_NE(d.headline.find("memory"), std::string::npos);
    EXPECT_NE(d.headline.find("CPU"), std::string::npos);
    auto idle = cpu_state(3.0, {}, 0.1);
    MemoryMetrics ok;
    ok.total_bytes = 16ULL << 30;
    ok.available_bytes = 10ULL << 30;
    idle.memory = Observation<MemoryMetrics>{ok, Provenance::now("t")};
    EXPECT_EQ(r.slowness(idle).conclusion, "no_current_bottleneck_observed");
}

TEST(ReasonConcept, ExplainsFromGraph) {
    Fixture fx;
    Reasoner r(fx.g, Platform::linux_os);
    auto d = r.explain_concept("cgroup_cpu_quota");
    EXPECT_FALSE(d.findings.empty());
    EXPECT_EQ(r.explain_concept("nonexistent").conclusion, "unknown_concept");
    auto j = to_json(d);
    EXPECT_NE(j.find("\"nature\":\"invariant\""), std::string::npos);
}
