// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "osie/state/state.hpp"
#include "osie/util/json.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using osie::test::MockOs;
using osie::test::proc;

TEST(Result, ValueAndError) {
    Result<int> ok = 3;
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(*ok, 3);
    Result<int> bad = make_error(Errc::permission_denied, "x", 1);
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, Errc::permission_denied);
    EXPECT_EQ(bad.value_or(7), 7);
    EXPECT_NE(bad.error().describe().find("permission_denied"), std::string::npos);
    EXPECT_EQ(errc_from_errno(EPERM), Errc::permission_denied);
    EXPECT_EQ(errc_from_errno(ESRCH), Errc::process_exited);
}

TEST(Json, EscapesAndStructure) {
    json::Writer w;
    w.begin_object().kv("a", std::string("q\"\n\x01")).key("b").begin_array().value(1).value(2.5).null().end_array()
        .kv("c", true).end_object();
    EXPECT_EQ(w.str(), R"({"a":"q\"\n\u0001","b":[1,2.5,null],"c":true})");
    json::Writer nan;
    nan.begin_array().value(std::nan("")).end_array();
    EXPECT_EQ(nan.str(), "[null]");
}

TEST(Provenance, Freshness) {
    auto p = Provenance::now("x");
    EXPECT_TRUE(p.fresh(Millis{1000}));
    EXPECT_FALSE(p.fresh(Millis{1000}, p.mono + Millis{1500}));
    p.validity = Validity::unavailable;
    EXPECT_FALSE(p.fresh(Millis{1000}));
}

TEST(Derive, CpuUtilization) {
    auto a = MockOs::cpu(100, 900);
    auto b = MockOs::cpu(200, 1000);  // +100 busy, +100 idle per CPU -> 50 %
    auto u = derive_cpu_utilization(a, b);
    ASSERT_TRUE(u);
    EXPECT_NEAR(u->busy_pct(), 50.0, 1e-9);
    ASSERT_EQ(u->per_cpu_busy_pct.size(), 4U);
    EXPECT_NEAR(u->per_cpu_busy_pct[2], 50.0, 1e-9);
    EXPECT_FALSE(u->iowait_pct);  // macOS-style sample: no iowait field
}

TEST(Derive, CpuCounterResetIsUnavailableNotNegative) {
    auto a = MockOs::cpu(500, 900);
    auto b = MockOs::cpu(100, 1000);
    EXPECT_FALSE(derive_cpu_utilization(a, b));
}

TEST(Derive, CpuHotplugSkipsPerCpu) {
    auto a = MockOs::cpu(100, 900, 4);
    auto b = MockOs::cpu(150, 950, 3);  // a CPU went offline
    b.aggregate.user = a.aggregate.user + 100;
    b.aggregate.idle = a.aggregate.idle + 100;
    auto u = derive_cpu_utilization(a, b);
    ASSERT_TRUE(u);
    EXPECT_TRUE(u->per_cpu_busy_pct.empty());
}

TEST(Derive, CounterRate) {
    EXPECT_NEAR(*counter_rate(10, 30, 2.0), 10.0, 1e-9);
    EXPECT_FALSE(counter_rate(30, 10, 2.0));           // wrap/reset
    EXPECT_FALSE(counter_rate(std::nullopt, 10, 2.0)); // missing
    EXPECT_FALSE(counter_rate(1, 2, 0.0));
}

TEST(Derive, ProcessCpuAndPidReuse) {
    ProcessSnapshot a, b;
    a.processes = {proc(10, 1, "busy", 1'000'000'000, 1), proc(11, 1, "reused", 5'000'000'000, 1)};
    auto busy2 = proc(10, 1, "busy", 1'500'000'000, 1);  // +0.5 s CPU over 0.5 s -> 100 %
    auto reused2 = proc(11, 1, "other", 10'000, 1, /*start=*/999'999);  // same pid, different start time
    b.processes = {busy2, reused2, proc(12, 1, "new", 1, 1)};
    auto v = derive_process_views(a, b, 0.5);
    ASSERT_EQ(v.size(), 3U);
    ASSERT_TRUE(v[0].cpu_pct);
    EXPECT_NEAR(*v[0].cpu_pct, 100.0, 1e-6);
    EXPECT_TRUE(v[1].new_in_interval) << "PID reuse must not be treated as the same process";
    EXPECT_FALSE(v[1].cpu_pct);
    EXPECT_TRUE(v[2].new_in_interval);
}

TEST(Derive, MemoryRates) {
    MemoryMetrics a, b;
    a.pageouts = 100;
    b.pageouts = 300;
    a.swapouts = 5;
    b.swapouts = 2;  // reset -> unavailable
    auto r = derive_memory_rates(a, b, 2.0);
    EXPECT_NEAR(*r.pageouts_per_s, 100.0, 1e-9);
    EXPECT_FALSE(r.swapouts_per_s);
    EXPECT_FALSE(r.pageins_per_s);
}

namespace {
struct Rig {
    MockOs os;
    CapabilityRegistry reg = CapabilityRegistry::builtin();
    SafetyPolicy pol;
    AuditLog audit;
    ObservationBroker broker{os, reg, pol, audit};
    explicit Rig(Platform p = Platform::macos) : os(p) {}
};
}  // namespace

TEST(Sampler, RecordsFailuresWithReasons) {
    Rig r;
    r.os.cpu_script = {MockOs::cpu(0, 100), MockOs::cpu(50, 150)};
    r.os.fail_memory_with_eperm = 1;
    StateSampler s(r.broker);
    auto st = s.sample(SampleRequest{{ObservationKind::cpu_times, ObservationKind::memory, ObservationKind::pressure_stall},
                                     Millis{10}});
    ASSERT_TRUE(st.cpu);
    EXPECT_NEAR(st.cpu->value.busy_pct(), 50.0, 1e-9);
    EXPECT_EQ(st.cpu->prov.validity, Validity::derived);
    EXPECT_FALSE(st.memory);
    const auto* mu = st.why_unavailable(ObservationKind::memory);
    ASSERT_NE(mu, nullptr);
    EXPECT_EQ(mu->why.code, Errc::permission_denied);
    const auto* pu = st.why_unavailable(ObservationKind::pressure_stall);
    ASSERT_NE(pu, nullptr);
    EXPECT_EQ(pu->why.code, Errc::unsupported) << "PSI on macOS is unsupported, not a policy denial";
}

TEST(Sampler, CancellationStopsSampling) {
    Rig r;
    r.os.cpu_script = {MockOs::cpu(0, 100)};
    CancellationSource src;
    src.cancel();
    StateSampler s(r.broker);
    auto st = s.sample(SampleRequest{{ObservationKind::cpu_times}, Millis{2000}}, src.token());
    EXPECT_FALSE(st.cpu);
    ASSERT_NE(st.why_unavailable(ObservationKind::cpu_times), nullptr);
    EXPECT_EQ(st.why_unavailable(ObservationKind::cpu_times)->why.code, Errc::cancelled);
}

TEST(Sampler, OnlyRequestedObservationsAreTaken) {
    Rig r;
    r.os.proc_script = {ProcessSnapshot{}};
    StateSampler s(r.broker);
    auto st = s.sample(SampleRequest{{ObservationKind::system_info}, Millis{10}});
    EXPECT_TRUE(st.system);
    EXPECT_EQ(r.os.process_calls, 0) << "a system-info question must not enumerate processes";
}

TEST(StateQueries, TopAndFind) {
    SystemState st;
    std::vector<ProcessView> v;
    v.push_back(ProcessView{proc(1, 0, "launchd", 0, 10), 1.0, false});
    v.push_back(ProcessView{proc(2, 1, "Google Chrome", 0, 500), 30.0, false});
    v.push_back(ProcessView{proc(3, 2, "Google Chrome Helper", 0, 900), 60.0, false});
    auto other = proc(4, 1, "root daemon", 0, 0);
    other.footprint_bytes.reset();
    other.resident_bytes.reset();
    other.validity = Validity::partial;
    v.push_back(ProcessView{other, std::nullopt, false});
    st.processes = Observation<std::vector<ProcessView>>{v, Provenance::now("t")};
    auto top = st.top_by_cpu(2);
    ASSERT_EQ(top.size(), 2U);
    EXPECT_EQ(top[0]->info.pid, 3);
    auto mem = st.top_by_memory(10);
    EXPECT_EQ(mem.size(), 3U) << "processes without readable memory are not ranked";
    auto f = st.find_by_name("google chrome");
    ASSERT_GE(f.size(), 2U);
    EXPECT_EQ(f[0]->info.pid, 2) << "exact name match ranks before partial matches";
    EXPECT_EQ(st.children_of(1).size(), 2U);
}
