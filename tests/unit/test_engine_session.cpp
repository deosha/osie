// SPDX-License-Identifier: Apache-2.0
// Engine + session context against a scripted mock OS (no real observations).
#include <gtest/gtest.h>

#include <thread>

#include "osie/engine/engine.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using osie::test::MockOs;
using osie::test::proc;

namespace {

ProcessSnapshot snap(std::vector<ProcessInfo> v) {
    ProcessSnapshot s;
    s.processes = std::move(v);
    return s;
}

std::unique_ptr<MockOs> busy_mac() {
    auto os = std::make_unique<MockOs>(Platform::macos);
    os->cpu_script = {MockOs::cpu(0, 1000), MockOs::cpu(300, 1100)};  // 75 % busy
    MemoryMetrics m;
    m.total_bytes = 16ULL << 30;
    m.available_bytes = 6ULL << 30;
    m.pressure_level = MemoryPressureLevel::normal;
    os->mem_script = {m};
    // before: cumulative CPU; after: +0.4 s for "render" (80 % over 0.5 s window)
    os->proc_script = {snap({proc(1, 0, "launchd", 0, 50 << 20), proc(200, 1, "render", 1'000'000'000, 3ULL << 30),
                             proc(300, 1, "editor", 0, 5ULL << 30)}),
                       snap({proc(1, 0, "launchd", 0, 50 << 20), proc(200, 1, "render", 1'400'000'000, 3ULL << 30),
                             proc(300, 1, "editor", 10'000'000, 5ULL << 30)})};
    return os;
}

EngineConfig fast() {
    EngineConfig c;
    c.sample_interval = Millis{10};
    c.freshness_budget = Millis{60'000};
    return c;
}

}  // namespace

TEST(Engine, SpecConversationResolvesReferences) {
    Engine e(busy_mac(), std::make_unique<intent::RuleParser>(), fast());
    auto a1 = e.ask("What is using CPU?");
    EXPECT_EQ(a1.frame.intent, intent::Intent::top_cpu);
    EXPECT_NE(a1.text.find("render"), std::string::npos);

    auto a2 = e.ask("Which one is using the most memory?");
    EXPECT_EQ(a2.frame.intent, intent::Intent::top_memory);
    EXPECT_NE(a2.text.find("editor"), std::string::npos);
    EXPECT_FALSE(a2.reused_observations) << "top_cpu did not observe memory, so it must be sampled";

    auto a3 = e.ask("Why might it be consuming so much memory?");
    EXPECT_EQ(a3.frame.intent, intent::Intent::process_memory);
    ASSERT_TRUE(a3.diagnosis);
    ASSERT_TRUE(a3.diagnosis->subject_pid);
    EXPECT_EQ(*a3.diagnosis->subject_pid, 300) << "'it' must resolve to the process from the previous answer";
    EXPECT_TRUE(a3.reused_observations) << "memory + processes from turn 2 are fresh and must be reused";

    auto a4 = e.ask("what about cpu?");
    EXPECT_EQ(a4.frame.intent, intent::Intent::process_cpu);
    ASSERT_TRUE(a4.diagnosis);
    EXPECT_EQ(*a4.diagnosis->subject_pid, 300);
}

TEST(Engine, OrdinalReference) {
    Engine e(busy_mac(), std::make_unique<intent::RuleParser>(), fast());
    e.ask("show the top processes by memory");
    auto a = e.ask("tell me about the second one");
    ASSERT_EQ(a.frame.target.kind, intent::TargetKind::reference);
    EXPECT_NE(a.text.find("render"), std::string::npos) << a.text;
}

TEST(Engine, ReferenceWithoutHistoryAsksForClarification) {
    Engine e(busy_mac(), std::make_unique<intent::RuleParser>(), fast());
    auto a = e.ask("why is it using so much memory");
    EXPECT_TRUE(a.frame.ambiguous);
    EXPECT_NE(a.text.find("Which process"), std::string::npos);
}

TEST(Engine, ProcessExitedBetweenTurns) {
    auto os = busy_mac();
    auto* raw = os.get();
    EngineConfig c = fast();
    c.freshness_budget = Millis{0};  // force re-observation each turn
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), c);
    e.ask("which app uses the most memory");  // -> editor (300)
    {
        std::lock_guard lk(raw->mu);
        raw->proc_script = {snap({proc(1, 0, "launchd", 0, 1), proc(200, 1, "render", 2'000'000'000, 1)})};
    }
    auto a = e.ask("why is it using so much memory?");
    EXPECT_TRUE(a.frame.ambiguous);
    EXPECT_NE(a.text.find("no longer running"), std::string::npos) << a.text;
}

TEST(Engine, ActionsAreRefusedAndAudited) {
    Engine e(busy_mac(), std::make_unique<intent::RuleParser>(), fast());
    auto a = e.ask("kill render");
    EXPECT_EQ(a.frame.intent, intent::Intent::action_request);
    EXPECT_NE(a.text.find("won't"), std::string::npos);
    auto rec = e.audit().recent(5);
    ASSERT_FALSE(rec.empty());
    EXPECT_EQ(rec.back().capability, "act.process.terminate");
    EXPECT_FALSE(rec.back().allowed);
    for (const auto& d : e.audit().recent(100))
        if (d.capability.rfind("act.", 0) == 0) EXPECT_FALSE(d.allowed);
}

TEST(Engine, UnsupportedAndKubernetes) {
    Engine e(busy_mac(), std::make_unique<intent::RuleParser>(), fast());
    EXPECT_NE(e.ask("write me a poem").text.find("outside"), std::string::npos);
    // An instruction is refused as an action; a question is "not implemented".
    EXPECT_NE(e.ask("scale my kubernetes deployment").text.find("won't"), std::string::npos);
    EXPECT_NE(e.ask("why is my kubernetes pod pending?").text.find("not implemented"), std::string::npos);
    auto c = e.ask("why is this container being throttled?");
    EXPECT_NE(c.text.find("Linux VM"), std::string::npos) << "macOS must not pretend to see container cgroups";
}

TEST(Engine, DiagnosisUsesGraphAndMarksUnavailable) {
    Engine e(busy_mac(), std::make_unique<intent::RuleParser>(), fast());
    auto a = e.ask("Why is my Mac using so much CPU?");
    ASSERT_TRUE(a.diagnosis);
    EXPECT_EQ(a.diagnosis->topic, "cpu");
    EXPECT_FALSE(a.diagnosis->evidence.empty());
    EXPECT_FALSE(a.frame.unavailable_observations.empty());  // PSI/cgroup on macOS
    EXPECT_GT(e.graph().stats().by_kind[static_cast<std::size_t>(arch::NodeKind::process)], 0U);
    EXPECT_TRUE(e.graph().check_consistency().empty());
}

TEST(StateCache, ConcurrentReadersAndWriter) {
    StateCache cache;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                if (auto s = cache.fresh({ObservationKind::cpu_times}, Millis{60'000})) {
                    EXPECT_TRUE(s->cpu.has_value());
                }
                ++reads;
            }
        });
    }
    // Keep writing until the readers have run: on a loaded machine 2000
    // stores can finish before any reader thread is scheduled.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int i = 0; i < 2000 || (reads.load() < 100 && std::chrono::steady_clock::now() < until); ++i) {
        auto s = std::make_shared<SystemState>();
        s->cpu = Observation<CpuUtilization>{CpuUtilization{}, Provenance::now("t")};
        s->taken_at = std::chrono::steady_clock::now();
        cache.store(s);
    }
    stop = true;
    for (auto& t : readers) t.join();
    EXPECT_GT(reads.load(), 0U);
}
