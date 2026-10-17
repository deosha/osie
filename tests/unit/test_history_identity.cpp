// SPDX-License-Identifier: Apache-2.0
// Process identity in history series: a reused PID is never stitched into
// the series of the process that held it before. Expected numbers come from
// the scenario's parameters.
#include <gtest/gtest.h>

#include "osie/engine/engine.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using osie::test::MockOs;
using Clock = std::chrono::steady_clock;

namespace {

/// Evidence for Engine::ask: a fixed state and a fixed history.
class FixedEvidence final : public EvidenceSource {
public:
    FixedEvidence(std::shared_ptr<const SystemState> s, StateHistory h) : s_(std::move(s)), h_(std::move(h)) {}
    std::shared_ptr<const SystemState> state(const std::vector<ObservationKind>&, bool, const CancellationToken&,
                                             bool& reused) override {
        reused = true;
        return s_;
    }
    const StateHistory& history() override { return h_; }

private:
    std::shared_ptr<const SystemState> s_;
    StateHistory h_;
};

ProcessPoint pp(Pid pid, std::optional<std::uint64_t> start, std::string name, double mib) {
    return ProcessPoint{pid, start, std::move(name), 1.0, static_cast<std::uint64_t>(mib * (1 << 20))};
}

/// 30 points one second apart. pid 100 is "nginx" growing 10 MiB/s for
/// t = 0..14, then (after it exits) "worker" at a constant 50 MiB for
/// t = 15..29. pid 200 "steady" holds 300 MiB throughout.
StateHistory reuse_history(std::optional<std::uint64_t> nginx_start, std::optional<std::uint64_t> worker_start) {
    const auto base = Clock::now() - std::chrono::seconds(31);
    const auto wbase = std::chrono::system_clock::now() - std::chrono::seconds(31);
    std::deque<HistoryPoint> d;
    for (int t = 0; t < 30; ++t) {
        HistoryPoint h;
        h.at = base + std::chrono::seconds(t);
        h.wall = wbase + std::chrono::seconds(t);
        h.seq = static_cast<std::uint64_t>(t + 1);
        h.cpu_busy_pct = 20;
        h.mem_total_bytes = 16ULL << 30;
        h.mem_available_bytes = 8ULL << 30;
        if (t < 15) h.processes.push_back(pp(100, nginx_start, "nginx", 100.0 + 10.0 * t));
        else h.processes.push_back(pp(100, worker_start, "worker", 50.0));
        h.processes.push_back(pp(200, 7, "steady", 300.0));
        d.push_back(std::move(h));
    }
    return StateHistory(HistoryConfig{720, 16, 10.0}, std::move(d));
}

std::shared_ptr<SystemState> current_state(std::optional<std::uint64_t> worker_start) {
    auto s = std::make_shared<SystemState>();
    s->taken_at = Clock::now();
    s->interval = Millis{200};
    MemoryMetrics m;
    m.total_bytes = 16ULL << 30;
    m.available_bytes = 8ULL << 30;
    s->memory = Observation<MemoryMetrics>{m, Provenance::now("t")};
    s->cpu = Observation<CpuUtilization>{CpuUtilization{}, Provenance::now("t", Validity::derived)};
    std::vector<ProcessView> v(2);
    v[0].info = osie::test::proc(100, 1, "worker", 0, 50ULL << 20);
    v[0].info.start_time_us = worker_start;
    v[0].cpu_pct = 1.0;
    v[1].info = osie::test::proc(200, 1, "steady", 0, 300ULL << 20);
    v[1].cpu_pct = 1.0;
    s->processes = Observation<std::vector<ProcessView>>{std::move(v), Provenance::now("t", Validity::derived)};
    return s;
}

void expect_nginx_alone(std::optional<std::uint64_t> nginx_start, std::optional<std::uint64_t> worker_start) {
    Engine e(std::make_unique<MockOs>(Platform::linux_os), std::make_unique<intent::RuleParser>());
    FixedEvidence ev(current_state(worker_start), reuse_history(nginx_start, worker_start));
    SessionContext session;
    const Answer a = e.ask("which process is growing in memory?", session, ev);
    ASSERT_EQ(a.trends.size(), a.trend_origins.size());
    bool found = false;
    for (std::size_t i = 0; i < a.trends.size(); ++i) {
        const auto& [label, t] = a.trends[i];
        const auto& o = a.trend_origins[i];
        ASSERT_TRUE(o.process) << label;
        EXPECT_NE(o.process->name, "worker") << "constant memory is not growth";
        if (o.process->name != "nginx") continue;
        found = true;
        // Expected from the parameters: 15 points 100..240 MiB; thirds of 5
        // points: early mean 120, late mean 220.
        EXPECT_EQ(t.direction, TrendDirection::increasing);
        EXPECT_EQ(t.n, 15U) << "only nginx's own points, none of the reused PID's";
        EXPECT_NEAR(t.early_mean, 120.0, 1e-6);
        EXPECT_NEAR(t.late_mean, 220.0, 1e-6);
        EXPECT_EQ(o.process->start_time_us, nginx_start);
        EXPECT_EQ(o.first_seq, 1U);
        EXPECT_EQ(o.last_seq, 15U);
    }
    EXPECT_TRUE(found) << a.text;
}

}  // namespace

TEST(HistoryIdentity, ReusedPidWithStartTimesIsASeparateSeries) { expect_nginx_alone(5000, 9000); }

// Without start times (a backend that could not read them), the name keeps
// the two processes apart rather than merging them under one PID.
TEST(HistoryIdentity, ReusedPidWithoutStartTimesIsSeparatedByName) { expect_nginx_alone(std::nullopt, std::nullopt); }

// A history series for the current subject never includes points of a
// different process that held the same PID.
TEST(HistoryIdentity, SubjectSeriesExcludesAnEarlierHolderOfThePid) {
    Engine e(std::make_unique<MockOs>(Platform::linux_os), std::make_unique<intent::RuleParser>());
    FixedEvidence ev(current_state(std::nullopt), reuse_history(std::nullopt, std::nullopt));
    SessionContext session;
    const Answer a = e.ask("is pid 100 using more memory over time?", session, ev);
    ASSERT_EQ(a.trends.size(), a.trend_origins.size());
    std::size_t process_series = 0;
    for (std::size_t i = 0; i < a.trends.size(); ++i) {
        const auto& o = a.trend_origins[i];
        if (!o.process) continue;
        ++process_series;
        EXPECT_EQ(o.process->name, "worker");
        EXPECT_EQ(a.trends[i].second.n, 15U) << "the worker held pid 100 for 15 points only";
        EXPECT_EQ(a.trends[i].second.direction, TrendDirection::stable);
        EXPECT_EQ(o.first_seq, 16U);
        EXPECT_EQ(o.last_seq, 30U);
    }
    EXPECT_EQ(process_series, 1U) << a.text;
}

// A process too new for a trend of its own is reported as such, never as
// "not growing". Ground truth: "steady" holds 300 MiB for 30 s; "python3"
// appears 5 s before the question and grows from 0 to 193 MiB in 6 samples,
// less than the 10 s a trend needs.
TEST(HistoryIdentity, ProcessTooNewToJudgeIsNotReportedAsNotGrowing) {
    const auto base = Clock::now() - std::chrono::seconds(31);
    const auto wbase = std::chrono::system_clock::now() - std::chrono::seconds(31);
    std::deque<HistoryPoint> d;
    for (int t = 0; t < 30; ++t) {
        HistoryPoint h;
        h.at = base + std::chrono::seconds(t);
        h.wall = wbase + std::chrono::seconds(t);
        h.seq = static_cast<std::uint64_t>(t + 1);
        h.cpu_busy_pct = 20;
        h.processes.push_back(pp(200, 7, "steady", 300.0));
        if (t >= 24) h.processes.push_back(pp(500, 11, "python3", 193.0 * (t - 24) / 5.0));
        d.push_back(std::move(h));
    }
    auto s = current_state(std::nullopt);
    {
        auto& v = s->processes->value;
        ProcessView py;
        py.info = osie::test::proc(500, 1, "python3", 0, 193ULL << 20);
        py.info.start_time_us = 11;
        py.cpu_pct = 5.0;
        v.push_back(py);
    }
    Engine e(std::make_unique<MockOs>(Platform::linux_os), std::make_unique<intent::RuleParser>());
    FixedEvidence ev(s, StateHistory(HistoryConfig{720, 16, 10.0}, std::move(d)));
    SessionContext session;
    const Answer a = e.ask("which process is growing in memory?", session, ev);
    EXPECT_EQ(a.text.find("none showed a clear increase"), std::string::npos) << a.text;
    EXPECT_NE(a.text.find("Too new to judge"), std::string::npos) << a.text;
    EXPECT_NE(a.text.find("python3 (pid 500, now 193 MiB, 6 samples"), std::string::npos) << a.text;
    EXPECT_EQ(a.too_new_to_judge, 1U);
    bool listed = false;
    for (std::size_t i = 0; i < a.trends.size(); ++i) {
        const auto& [label, t] = a.trends[i];
        if (label.find("python3") == std::string::npos) continue;
        listed = true;
        EXPECT_EQ(t.direction, TrendDirection::insufficient_data);
        EXPECT_EQ(t.n, 6U);
        EXPECT_NEAR(t.span_s, 5.0, 1e-6);
        ASSERT_TRUE(a.trend_origins[i].process);
        EXPECT_EQ(a.trend_origins[i].process->pid, 500);
    }
    EXPECT_TRUE(listed);
    EXPECT_EQ(a.text.find("steady (pid 200)"), std::string::npos) << "constant memory is neither growth nor too new";
}
