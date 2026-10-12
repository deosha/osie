// SPDX-License-Identifier: Apache-2.0
// Temporal intelligence: cue detection, bounded history, trend statistics
// and engine answers built only from recorded samples.
#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <thread>

#include "osie/engine/engine.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using osie::test::MockOs;
using osie::test::proc;
using intent::Intent;
using intent::TemporalKind;
using intent::TemporalMetric;

namespace {

Series line(std::size_t n, double start, double step, double noise, unsigned seed = 7) {
    std::mt19937 g(seed);
    std::normal_distribution<double> d(0.0, noise);
    Series s;
    for (std::size_t i = 0; i < n; ++i) s.emplace_back(static_cast<double>(i) * 5.0, start + step * static_cast<double>(i) + d(g));
    return s;
}

ProcessSnapshot snap(std::vector<ProcessInfo> v) {
    ProcessSnapshot s;
    s.processes = std::move(v);
    return s;
}

/// Mock whose k-th sample shows `busy[k]` % CPU, memory pressure `pressure[k]`
/// and pid 11 ("grower") at `grower_mib[k]` MiB.
std::unique_ptr<MockOs> scripted(const std::vector<std::uint64_t>& busy, const std::vector<MemoryPressureLevel>& pressure,
                                 const std::vector<std::uint64_t>& grower_mib) {
    auto os = std::make_unique<MockOs>(Platform::macos);
    std::uint64_t b = 0, i = 0;
    for (auto x : busy) {
        os->cpu_script.push_back(MockOs::cpu(b, i));
        b += x;
        i += 100 - x;
        os->cpu_script.push_back(MockOs::cpu(b, i));
    }
    for (auto lvl : pressure) {
        MemoryMetrics m;
        m.total_bytes = 16ULL << 30;
        m.available_bytes = 8ULL << 30;
        m.pressure_level = lvl;
        os->mem_script.push_back(m);
        os->mem_script.push_back(m);
    }
    std::uint64_t cpu_ns = 0;
    for (auto mib : grower_mib) {
        for (int k = 0; k < 2; ++k) {
            os->proc_script.push_back(snap({proc(1, 0, "launchd", 0, 10 << 20), proc(11, 1, "grower", cpu_ns, mib << 20),
                                            proc(12, 1, "steady", 0, 300ULL << 20)}));
            cpu_ns += 1'000'000;
        }
    }
    return os;
}

EngineConfig fast() {
    EngineConfig c;
    c.sample_interval = Millis{2};
    c.freshness_budget = Millis{60'000};
    c.history.min_span_s = 0.0;  // samples are milliseconds apart in tests
    return c;
}

}  // namespace

// ------------------------------------------------------------ detection ---

TEST(TemporalCues, BriefQuestionsAreDetected) {
    struct Case {
        const char* text;
        TemporalKind kind;
        TemporalMetric metric;
        std::optional<std::int64_t> window;
        bool rank;
    };
    const Case cases[] = {
        {"Has CPU usage increased in the last five minutes?", TemporalKind::trend, TemporalMetric::cpu, 300, false},
        {"Which process has been growing in memory?", TemporalKind::trend, TemporalMetric::memory, std::nullopt, true},
        {"When did memory pressure become elevated?", TemporalKind::onset, TemporalMetric::memory_pressure, std::nullopt, false},
        {"Is the system recovering?", TemporalKind::recovery, TemporalMetric::overall, std::nullopt, false},
        {"Has its usage increased?", TemporalKind::trend, TemporalMetric::overall, std::nullopt, false},
        {"is memory usage going up over the past hour", TemporalKind::trend, TemporalMetric::memory, 3600, false},
    };
    for (const auto& c : cases) {
        auto q = intent::detect_temporal(intent::tokenize(c.text));
        ASSERT_TRUE(q) << c.text;
        EXPECT_EQ(q->kind, c.kind) << c.text;
        EXPECT_EQ(q->metric, c.metric) << c.text;
        EXPECT_EQ(q->window_s, c.window) << c.text;
        EXPECT_EQ(q->rank_processes, c.rank) << c.text;
    }
}

TEST(TemporalCues, NonTemporalQuestionsAreUntouched) {
    MockOs os;
    const auto reg = CapabilityRegistry::builtin();
    intent::RuleParser rp;
    for (const char* t : {"what is using the most cpu", "what started chrome", "show the top 3 processes",
                          "why is my mac slow", "increase the priority of chrome", "kill the last one"}) {
        const auto toks = intent::tokenize(t);
        auto f = intent::build_frame(rp.parse(toks), toks, Platform::macos, os.supported(), reg);
        EXPECT_FALSE(f.temporal) << t;
    }
}

TEST(TemporalCues, ActionsAreNeverTemporal) {
    MockOs os;
    const auto reg = CapabilityRegistry::builtin();
    intent::RuleParser rp;
    const auto toks = intent::tokenize("increase the priority of chrome over time");
    auto f = intent::build_frame(rp.parse(toks), toks, Platform::macos, os.supported(), reg);
    EXPECT_EQ(f.intent, Intent::action_request);
    EXPECT_FALSE(f.temporal);
}

TEST(TemporalCues, TimePhraseIsNotAReference) {
    MockOs os;
    const auto reg = CapabilityRegistry::builtin();
    intent::RuleParser rp;
    const auto toks = intent::tokenize("Has CPU usage increased in the last five minutes?");
    auto f = intent::build_frame(rp.parse(toks), toks, Platform::macos, os.supported(), reg);
    EXPECT_EQ(f.target.kind, intent::TargetKind::host) << "'the last' must not become 'the last one'";
    EXPECT_EQ(f.intent, Intent::diagnose_cpu);
}

// ----------------------------------------------------------- statistics ---

TEST(Trend, DetectsIncreaseDecreaseAndStability) {
    EXPECT_EQ(analyse_trend(line(30, 20, 1.0, 2.0)).direction, TrendDirection::increasing);
    EXPECT_EQ(analyse_trend(line(30, 80, -1.0, 2.0)).direction, TrendDirection::decreasing);
    EXPECT_EQ(analyse_trend(line(30, 40, 0.0, 3.0)).direction, TrendDirection::stable);
}

TEST(Trend, TinyMonotonicDriftIsNotReportedAsAChange) {
    // Perfectly monotonic (z is large) but only +0.6 pp overall: below the effect floor.
    const auto r = analyse_trend(line(30, 40, 0.02, 0.0));
    EXPECT_GT(r.mann_kendall_z, 1.96);
    EXPECT_EQ(r.direction, TrendDirection::stable);
}

TEST(Trend, TooFewOrTooShortIsInsufficient) {
    EXPECT_EQ(analyse_trend(line(3, 0, 10, 0)).direction, TrendDirection::insufficient_data);
    Series short_span = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}};
    const auto r = analyse_trend(short_span);
    EXPECT_EQ(r.direction, TrendDirection::insufficient_data);
    EXPECT_FALSE(r.why.empty());
    EXPECT_EQ(analyse_trend({}).direction, TrendDirection::insufficient_data);
}

TEST(Trend, SlopeIsPerMinute) {
    const auto r = analyse_trend(line(13, 0, 1.0, 0.0));  // +1 per 5 s = +12 per minute
    EXPECT_NEAR(r.slope_per_min, 12.0, 1e-9);
}

// -------------------------------------------------------------- history ---

TEST(History, IsBoundedAndOrdered) {
    HistoryConfig hc;
    hc.capacity = 8;
    StateHistory h(hc);
    for (int i = 0; i < 50; ++i) {
        SystemState s;
        s.taken_at = SteadyTime{} + std::chrono::seconds(i);
        CpuUtilization u;
        u.idle_pct = 50;
        s.cpu = Observation<CpuUtilization>{u, Provenance::now("t")};
        h.record(s);
        h.record(s);  // re-recording the same state is ignored
    }
    EXPECT_EQ(h.size(), 8U);
    for (std::size_t i = 1; i < h.points().size(); ++i) EXPECT_LT(h.points()[i - 1].at, h.points()[i].at);
    EXPECT_GT(h.approx_bytes(), 0U);
}

// --------------------------------------------------------------- engine ---

TEST(TemporalEngine, NoHistoryMeansNoTrendClaim) {
    auto e = Engine(scripted({10}, {MemoryPressureLevel::normal}, {100}), std::make_unique<intent::RuleParser>(), [] {
        EngineConfig c = fast();
        c.history.min_span_s = 10.0;
        return c;
    }());
    auto a = e.ask("has cpu usage increased?");
    ASSERT_TRUE(a.frame.temporal);
    ASSERT_EQ(a.trends.size(), 1U);
    EXPECT_EQ(a.trends[0].second.direction, TrendDirection::insufficient_data);
    EXPECT_NE(a.text.find("not enough history"), std::string::npos) << a.text;
    EXPECT_NE(a.text.find("--watch"), std::string::npos);
}

TEST(TemporalEngine, RisingCpuIsReportedAsIncreasing) {
    std::vector<std::uint64_t> busy;
    for (int k = 0; k < 12; ++k) busy.push_back(static_cast<std::uint64_t>(10 + 6 * k));
    Engine e(scripted(busy, {MemoryPressureLevel::normal}, {100}), std::make_unique<intent::RuleParser>(), fast());
    for (int k = 0; k < 11; ++k) e.record_sample();
    auto a = e.ask("has cpu usage increased?");
    ASSERT_FALSE(a.trends.empty());
    EXPECT_EQ(a.trends[0].second.direction, TrendDirection::increasing) << a.text;
    EXPECT_NE(a.text.find("increasing"), std::string::npos);
}

TEST(TemporalEngine, RecoveryAfterPeak) {
    const std::vector<std::uint64_t> busy = {20, 40, 70, 90, 95, 90, 70, 50, 30, 20, 15, 12};
    Engine e(scripted(busy, {MemoryPressureLevel::normal}, {100}), std::make_unique<intent::RuleParser>(), fast());
    for (std::size_t k = 0; k + 1 < busy.size(); ++k) e.record_sample();
    auto a = e.ask("is cpu usage recovering?");
    ASSERT_TRUE(a.frame.temporal);
    EXPECT_EQ(a.frame.temporal->kind, TemporalKind::recovery);
    EXPECT_NE(a.text.find("recovering: down"), std::string::npos) << a.text;
}

TEST(TemporalEngine, OnsetOfMemoryPressure) {
    using L = MemoryPressureLevel;
    Engine e(scripted({10}, {L::normal, L::normal, L::normal, L::warning, L::warning, L::warning}, {100}),
             std::make_unique<intent::RuleParser>(), fast());
    for (int k = 0; k < 5; ++k) e.record_sample();
    auto a = e.ask("when did memory pressure become elevated?");
    EXPECT_NE(a.text.find("began at"), std::string::npos) << a.text;
    EXPECT_NE(a.text.find("still present"), std::string::npos) << a.text;
}

TEST(TemporalEngine, OnsetBeforeRecordingIsUnknown) {
    Engine e(scripted({10}, {MemoryPressureLevel::critical}, {100}), std::make_unique<intent::RuleParser>(), fast());
    for (int k = 0; k < 5; ++k) e.record_sample();
    auto a = e.ask("when did memory pressure become elevated?");
    EXPECT_NE(a.text.find("already present when recording began"), std::string::npos) << a.text;
}

TEST(TemporalEngine, GrowingProcessIsRankedAndFollowUpUsesMemory) {
    std::vector<std::uint64_t> mib;
    for (int k = 0; k < 12; ++k) mib.push_back(static_cast<std::uint64_t>(100 + 40 * k));
    Engine e(scripted({10}, {MemoryPressureLevel::normal}, mib), std::make_unique<intent::RuleParser>(), fast());
    for (int k = 0; k < 10; ++k) e.record_sample();
    auto a = e.ask("which process has been growing in memory?");
    EXPECT_NE(a.text.find("1. grower (pid 11)"), std::string::npos) << a.text;
    EXPECT_EQ(a.text.find("steady"), std::string::npos) << "a flat process is not listed";

    e.ask("which app uses the most memory?");  // grower is now the largest
    auto b = e.ask("has its usage increased?");
    EXPECT_EQ(b.frame.intent, Intent::process_memory) << "follow-up inherits the memory focus";
    ASSERT_FALSE(b.trends.empty());
    EXPECT_EQ(b.trends[0].second.direction, TrendDirection::increasing) << b.text;
}

TEST(TemporalEngine, ConcurrentWatcherAndQuestions) {
    // The --watch sampler records from another thread while questions are
    // answered; run under TSan in CI.
    std::vector<std::uint64_t> busy(40, 30);
    Engine e(scripted(busy, {MemoryPressureLevel::normal}, {100}), std::make_unique<intent::RuleParser>(), fast());
    std::atomic<bool> stop{false};
    std::thread w([&] {
        while (!stop) e.record_sample();
    });
    for (int i = 0; i < 20; ++i) (void)e.ask(i % 2 ? "has cpu usage increased?" : "what is using cpu");
    stop = true;
    w.join();
    EXPECT_GT(e.history_size(), 2U);
}
