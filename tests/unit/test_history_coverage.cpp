// SPDX-License-Identifier: Apache-2.0
// Historical diagnosis over the bounded history: the window asked for
// against what the history holds, gaps and suspend. Every expected number is computed from the scenario's parameters
// (point spacing, gap lengths, the window), independently of the code.
#include <gtest/gtest.h>

#include <cmath>

#include "osie/state/history.hpp"

using namespace osie;
using Clock = std::chrono::steady_clock;

namespace {

/// One point per entry: monotonic offset and wall offset from a common base
/// (seconds), and its snapshot sequence number.
struct P {
    double mono, wall;
    std::uint64_t seq{0};
};

struct Synthetic {
    SteadyTime base = Clock::now() - std::chrono::hours(48);  // far enough back for any offset
    WallTime wall_base = std::chrono::system_clock::now() - std::chrono::hours(48);
    SteadyTime at(double s) const { return base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }
    WallTime wall(double s) const {
        return wall_base + std::chrono::duration_cast<WallTime::duration>(std::chrono::duration<double>(s));
    }
    StateHistory history(const std::vector<P>& pts, std::size_t capacity = 100000) const {
        std::deque<HistoryPoint> d;
        for (const auto& p : pts) {
            HistoryPoint h;
            h.at = at(p.mono);
            h.wall = wall(p.wall);
            h.seq = p.seq;
            h.cpu_busy_pct = 10;
            d.push_back(h);
        }
        return StateHistory(HistoryConfig{capacity, 16, 10.0}, std::move(d));
    }
};

std::vector<P> every(double step, double from, double to, std::uint64_t seq0 = 1) {
    std::vector<P> v;
    std::uint64_t seq = seq0;
    for (double t = from; t <= to + 1e-9; t += step) v.push_back({t, t, seq++});
    return v;
}

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

}  // namespace

// A continuous history at a 5 s interval, asked about the last 10 minutes.
TEST(HistoryCoverage, ContinuousHistoryCoversTheWindow) {
    Synthetic sx;
    const auto h = sx.history(every(5, 0, 600));  // 121 points, t = 0 .. 600
    // now = 601: points with now - t <= 600 are t = 5 .. 600 (120 points).
    const auto c = history_coverage(h, sx.at(601), sx.wall(601), 600.0, 12.0);
    EXPECT_EQ(c.points, 120U);
    EXPECT_TRUE(near(c.observed_span_s, 595.0, 1e-3));
    EXPECT_EQ(c.first_seq, 2U);
    EXPECT_EQ(c.last_seq, 121U);
    EXPECT_TRUE(c.gaps.empty()) << "4 s before the first point and 1 s after the last are within the 12 s threshold";
    ASSERT_TRUE(c.coverage);
    EXPECT_TRUE(near(*c.coverage, 1.0));
    EXPECT_EQ(c.history_points, 121U);
}

// "In the last 10 minutes" when the history holds one minute.
TEST(HistoryCoverage, WindowLongerThanTheHistory) {
    Synthetic sx;
    const auto h = sx.history(every(5, 0, 60));  // 13 points
    const auto c = history_coverage(h, sx.at(60.5), sx.wall(60.5), 600.0, 12.0);
    EXPECT_EQ(c.points, 13U);
    EXPECT_TRUE(near(c.observed_span_s, 60.0, 1e-3));
    ASSERT_EQ(c.gaps.size(), 1U);
    EXPECT_EQ(c.gaps[0].kind, HistoryGap::Kind::before_first_point);
    // The window began 600 s before now (60.5): 539.5 s before the first point.
    EXPECT_TRUE(near(c.gaps[0].seconds, 539.5, 1e-3)) << c.gaps[0].seconds;
    ASSERT_TRUE(c.coverage);
    EXPECT_TRUE(near(*c.coverage, 1.0 - 539.5 / 600.0, 1e-5)) << *c.coverage;
    EXPECT_LT(*c.coverage, kMinHistoryCoverage);
}

// The agent stopped for 100 s in the middle of the window.
TEST(HistoryCoverage, CollectionGapIsUnobservedNotInterpolated) {
    Synthetic sx;
    auto pts = every(5, 0, 100, 1);
    const auto later = every(5, 200, 300, 22);
    pts.insert(pts.end(), later.begin(), later.end());
    const auto h = sx.history(pts);
    const auto c = history_coverage(h, sx.at(300.5), sx.wall(300.5), 300.0, 12.0);
    EXPECT_EQ(c.points, 20U + 21U) << "t = 5 .. 100 and 200 .. 300";
    ASSERT_EQ(c.gaps.size(), 1U);
    EXPECT_EQ(c.gaps[0].kind, HistoryGap::Kind::collection_gap);
    EXPECT_TRUE(near(c.gaps[0].seconds, 100.0, 1e-3));
    EXPECT_EQ(c.gaps[0].from, sx.wall(100));
    EXPECT_EQ(c.gaps[0].to, sx.wall(200));
    ASSERT_TRUE(c.coverage);
    EXPECT_TRUE(near(*c.coverage, 1.0 - 100.0 / 300.0, 1e-5)) << *c.coverage;
}

// A gap exactly at the threshold is normal spacing; just above it is a gap.
TEST(HistoryCoverage, ThresholdIsStrict) {
    Synthetic sx;
    const auto h = sx.history({{0, 0}, {12, 12}, {24.5, 24.5}});
    const auto c = history_coverage(h, sx.at(24.5), sx.wall(24.5), std::nullopt, 12.0);
    ASSERT_EQ(c.gaps.size(), 1U);
    EXPECT_TRUE(near(c.gaps[0].seconds, 12.5, 1e-3));
}

// The machine was suspended for an hour: the monotonic clock did not
// advance, the wall clock did. The suspended hour is unobserved.
TEST(HistoryCoverage, SuspendIsAGapEvenThoughTheMonotonicClockStopped) {
    Synthetic sx;
    std::vector<P> pts;
    for (int i = 0; i <= 20; ++i) {
        const double m = 5.0 * i;
        pts.push_back({m, i <= 10 ? m : m + 3600.0, static_cast<std::uint64_t>(i + 1)});
    }
    const auto h = sx.history(pts);
    const auto c = history_coverage(h, sx.at(101), sx.wall(101 + 3600), std::nullopt, 12.0);
    ASSERT_EQ(c.gaps.size(), 1U);
    EXPECT_EQ(c.gaps[0].kind, HistoryGap::Kind::suspended_or_clock_change);
    EXPECT_TRUE(near(c.gaps[0].seconds, 3600.0, 1e-3)) << c.gaps[0].seconds;
    EXPECT_EQ(c.gaps[0].from, sx.wall(50));
    EXPECT_EQ(c.gaps[0].to, sx.wall(55 + 3600));
    ASSERT_TRUE(c.coverage);
    // Reference: 101 s since the first point on the monotonic clock plus the
    // suspended hour.
    EXPECT_TRUE(near(*c.coverage, 1.0 - 3600.0 / 3701.0, 1e-5)) << *c.coverage;
}

// The agent stopped collecting 50 s ago.
TEST(HistoryCoverage, NoRecentPointsIsATrailingGap) {
    Synthetic sx;
    const auto h = sx.history(every(5, 0, 100));
    const auto c = history_coverage(h, sx.at(150), sx.wall(150), 120.0, 12.0);
    ASSERT_FALSE(c.gaps.empty());
    EXPECT_EQ(c.gaps.back().kind, HistoryGap::Kind::after_last_point);
    EXPECT_TRUE(near(c.gaps.back().seconds, 50.0, 1e-3));
    // Window [30, 150]: points 30 .. 100; 50 s after the last are unobserved.
    EXPECT_EQ(c.points, 15U);
    EXPECT_TRUE(near(*c.coverage, 1.0 - 50.0 / 120.0, 1e-5)) << *c.coverage;
}

TEST(HistoryCoverage, EmptyHistory) {
    Synthetic sx;
    const auto h = sx.history({});
    const auto w = history_coverage(h, sx.at(10), sx.wall(10), 60.0, 12.0);
    EXPECT_EQ(w.points, 0U);
    ASSERT_EQ(w.gaps.size(), 1U);
    EXPECT_EQ(w.gaps[0].kind, HistoryGap::Kind::before_first_point);
    EXPECT_TRUE(near(w.gaps[0].seconds, 60.0));
    EXPECT_TRUE(w.coverage && near(*w.coverage, 0.0));
    const auto all = history_coverage(h, sx.at(10), sx.wall(10), std::nullopt, 12.0);
    EXPECT_FALSE(all.coverage) << "no points and no window: nothing to compare";
    EXPECT_TRUE(all.gaps.empty());
}

// Snapshot sequence numbers recorded with each point.
TEST(HistoryCoverage, PointsRecordTheirSnapshot) {
    StateHistory h(HistoryConfig{8, 4, 10.0});
    SystemState s;
    s.taken_at = Clock::now();
    s.cpu = Observation<CpuUtilization>{CpuUtilization{}, Provenance::now("t")};
    h.record(s, {}, 41);
    s.taken_at += Millis{100};
    h.record(s, {}, 42);
    ASSERT_EQ(h.size(), 2U);
    EXPECT_EQ(h.points()[0].seq, 41U);
    EXPECT_EQ(h.points()[1].seq, 42U);
}
