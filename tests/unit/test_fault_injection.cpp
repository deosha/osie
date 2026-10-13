// SPDX-License-Identifier: Apache-2.0
// Fault injection (v0.2 brief, section 22): every failure returns a
// structured, explained answer and never crashes. Cases already covered
// elsewhere: counter resets and CPU hotplug (test_core_state), PID reuse and
// ambiguous names (test_identity_safety), unsupported model versions and
// incomplete history (test_prediction), quota change and counter reset in a
// cgroup window (test_language_safety).
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "osie/engine/engine.hpp"
#include "osie/inference/neural_parser.hpp"
#include "osie/prediction/world_model.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using intent::Intent;
using osie::test::MockOs;
using osie::test::proc;

namespace {

#if defined(OSIE_SANITIZED)
constexpr bool kSanitized = true;  // 10-100x slower: wall-clock bounds are not meaningful
#else
constexpr bool kSanitized = false;
#endif

EngineConfig fast() {
    EngineConfig c;
    c.sample_interval = Millis{5};
    c.experimental_prediction = true;
    return c;
}

std::unique_ptr<MockOs> base(Platform p = Platform::linux_os) {
    auto os = std::make_unique<MockOs>(p);
    os->info.arch = "x86_64";
    os->info.kernel_release = "6.8.0";
    os->cpu_script = {MockOs::cpu(0, 1000), MockOs::cpu(100, 1100)};
    MemoryMetrics m;
    m.total_bytes = 8ULL << 30;
    os->mem_script = {m};
    return os;
}

CgroupInfo cg(std::uint64_t usage, std::uint64_t periods, std::uint64_t throttled, std::uint64_t throttled_us) {
    CgroupInfo c;
    c.path = "/app";
    c.usage_usec = usage;
    c.nr_periods = periods;
    c.nr_throttled = throttled;
    c.throttled_usec = throttled_us;
    c.cpu_quota_us = 100000;
    c.cpu_period_us = 100000;
    c.cpuset_cpus = 4;
    c.pids_current = 3;
    return c;
}

}  // namespace

TEST(FaultInjection, ProcessExitsDuringSampling) {
    auto os = base();
    ProcessSnapshot a, b;
    a.processes = {proc(1, 0, "init", 0, 1 << 20), proc(50, 1, "shortlived", 0, 1 << 20)};
    b.processes = {proc(1, 0, "init", 0, 1 << 20)};
    b.vanished_count = 1;
    os->proc_script = {a, b};
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
    const auto ans = e.ask("what processes are running?");
    EXPECT_NE(ans.text.find("exited while the list was being read"), std::string::npos) << ans.text;
    const auto info = e.ask("tell me about pid 50");
    EXPECT_NE(info.text.find("No running process has PID 50"), std::string::npos) << info.text;
}

TEST(FaultInjection, ProcessListFailureIsReported) {
    for (auto err : {Errc::permission_denied, Errc::unavailable, Errc::io_error}) {
        auto os = base();
        os->process_list_error = err;
        Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
        const auto a = e.ask("what processes are running?");
        EXPECT_NE(a.text.find("unavailable"), std::string::npos) << a.text;
        EXPECT_FALSE(a.text.empty());
    }
}

TEST(FaultInjection, MemoryPermissionDeniedIsExplained) {
    auto os = base(Platform::macos);
    os->fail_memory_with_eperm = 1;
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
    const auto a = e.ask("how much memory is free");
    EXPECT_NE(a.text.find("unavailable"), std::string::npos) << a.text;
}

TEST(FaultInjection, MissingPsiIsNamedAsMissingEvidence) {
    auto os = base();  // MockOs returns 'unsupported' for PSI when none is scripted
    ProcessSnapshot s;
    s.processes = {proc(1, 0, "init", 0, 1 << 20)};
    os->proc_script = {s};
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
    const auto a = e.ask("why is the cpu busy");
    ASSERT_TRUE(a.diagnosis);
    bool named = false;
    for (const auto& m : a.diagnosis->missing_evidence) named = named || m.find("PSI") != std::string::npos;
    EXPECT_TRUE(named) << a.text;
}

TEST(FaultInjection, CgroupDisappearsDuringPrediction) {
    auto os = base();
    ProcessSnapshot s;
    auto p = proc(42, 1, "api", 0, 100 << 20);
    p.cgroup = "/app";
    s.processes = {p};
    os->proc_script = {s};
    for (int i = 0; i < 10; ++i) os->cgroup_script.push_back(cg(50'000u * i, 5u * i, 0, 0));
    os->cgroup_ok_calls = 2;  // the cgroup is removed after two reads
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
    std::string text =
        "OSIEWM2\nid t\nkind linear\nhistory 6\nwindow_s 0.05\nfeatures m_mech_usage_after\nmean 0\nscale 1\n"
        "w2 1 0 0 0\nb2 0 0 0 0\ntarget usage_cores cores identity 0 cap\ntarget throttled_cores cores identity 0 64\n"
        "target throttled_ratio fraction identity 0 1\ntarget psi_some fraction identity 0 1\n"
        "conformal usage_cores 0.8 0.1\nconformal throttled_cores 0.8 0.1\nconformal throttled_ratio 0.8 0.1\n"
        "conformal psi_some 0.8 0.1\nood_lo -1\nood_hi 10\nood_score_max 50\naction cpu_quota 0.5 3\n"
        "support_arch x86_64\nsupport_kernel 6.1 6.12\nfeature_schema 1\nmodel_version a\ndata_version b\n"
        "calibration_version c\nsupport_cgroup 2\nsupport_throttle_semantics summed_per_cpu\nsupport_capacity 0.5 3\n"
        "workload_coverage arith\nend\n";
    auto m = predict::WorldModel::load_from_memory(text);
    ASSERT_TRUE(m.ok()) << m.error().message;
    e.set_world_model(*m);
    const auto a = e.predict(PredictTarget{42, "", ""}, predict::ActionKind::cpu_quota, 2.0);
    EXPECT_FALSE(a.answered);
    EXPECT_FALSE(a.text.empty());
    // Either no valid window, or fewer windows than trained with (withheld).
    if (a.prediction) EXPECT_TRUE(a.prediction->abstained) << a.text;
}

TEST(FaultInjection, CgroupDisappearsDuringRateWindow) {
    auto os = base();
    ProcessSnapshot s;
    auto p = proc(42, 1, "api", 0, 100 << 20);
    p.cgroup = "/app";
    s.processes = {p};
    auto after = s;
    after.processes[0].cpu_user_ns = 5'000'000;
    os->proc_script = {s, after};
    os->cgroup_script = {cg(0, 0, 0, 0)};
    os->cgroup_ok_calls = 1;
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
    const auto a = e.ask("which cgroup is constrained?");
    EXPECT_EQ(a.frame.intent, Intent::cgroup_status);
    EXPECT_NE(a.text.find("disappeared during sampling"), std::string::npos) << a.text;
}

TEST(FaultInjection, CorruptModelFilesFailCleanly) {
    for (const char* name : {"world_cpu.osiewm", "intent.osiem"}) {
        const std::string path = std::string(OSIE_SOURCE_DIR) + "/models/" + name;
        std::ifstream in(path, std::ios::binary);
        if (!in) GTEST_SKIP() << "model not present: " << path;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string full = ss.str();
        std::size_t rejected = 0, n = 0;
        const std::size_t cuts = kSanitized ? 13 : 97, flips = kSanitized ? 17 : 211;
        for (std::size_t cut = 0; cut < full.size(); cut += std::max<std::size_t>(1, full.size() / cuts)) {
            const std::string_view part(full.data(), cut);
            ++n;
            if (std::string(name).find("osiewm") != std::string::npos) rejected += !predict::WorldModel::load_from_memory(part).ok();
            else rejected += !inference::IntentModel::load_from_memory(part).ok();
        }
        EXPECT_EQ(rejected, n) << name << ": every truncation must be rejected";
        // Bit flips anywhere: either rejected or loads; never crashes.
        for (std::size_t i = 0; i < full.size(); i += std::max<std::size_t>(1, full.size() / flips)) {
            std::string flipped = full;
            flipped[i] = static_cast<char>(flipped[i] ^ 0x5A);
            if (std::string(name).find("osiewm") != std::string::npos) (void)predict::WorldModel::load_from_memory(flipped);
            else (void)inference::IntentModel::load_from_memory(flipped);
        }
    }
}

TEST(FaultInjection, HugeAndHostileInputsAreBounded) {
    Engine e(base(), std::make_unique<intent::RuleParser>(), fast());
    std::string big(1 << 20, 'a');
    for (std::size_t i = 0; i < big.size(); i += 7) big[i] = ' ';
    const auto t0 = std::chrono::steady_clock::now();
    const auto f = e.interpret(big);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(f.read_only);
    if (!kSanitized) EXPECT_LT(ms, 5000.0) << "1 MiB utterance must be processed in bounded time";
    for (const char* s : {"\xff\xfe\xfd", "kill \0 9", "\"\"\"''''", "?????", "pid 99999999999999999999",
                          "top 999999999999 processes", "kill -9 -1"}) {
        const auto g = e.interpret(s);
        EXPECT_TRUE(g.read_only);
        EXPECT_TRUE(intent::validate(g).empty() || g.intent == Intent::action_request) << s;
    }
    // "kill -9 -1" (signal every process) is a directive, and refused.
    EXPECT_EQ(e.interpret("kill -9 -1").intent, Intent::action_request);
}
