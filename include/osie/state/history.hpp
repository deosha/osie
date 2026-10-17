// SPDX-License-Identifier: Apache-2.0
// Bounded in-memory history of observations, and simple trend statistics.
//
// OSIE has no background daemon. History is recorded only when OSIE itself
// samples: when it answers a question, or - if the user opts in with
// --watch - at a fixed interval while a session is open. Nothing is
// persisted to disk. Memory is bounded by `capacity` points of at most
// 2 * top_k (+ pinned) processes each.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "osie/state/state.hpp"

namespace osie {

struct ProcessPoint {
    Pid pid{0};
    std::optional<std::uint64_t> start_time_us;  // identity is (pid, start time)
    std::string name;
    std::optional<double> cpu_pct;
    std::optional<std::uint64_t> memory_bytes;
};

struct HistoryPoint {
    SteadyTime at{};
    std::chrono::system_clock::time_point wall{};
    /// Sequence number of the agent snapshot this point was recorded from;
    /// 0 when it was not recorded from a published snapshot (the CLI).
    std::uint64_t seq{0};
    std::optional<double> cpu_busy_pct;
    std::optional<std::uint64_t> mem_available_bytes;
    std::optional<std::uint64_t> mem_total_bytes;
    std::optional<MemoryPressureLevel> pressure;  // macOS kernel level
    std::optional<double> psi_cpu_some10, psi_mem_some10;  // Linux PSI avg10
    std::vector<ProcessPoint> processes;  // top-k by CPU and by memory, plus pinned identities
};

struct HistoryConfig {
    std::size_t capacity{720};  // e.g. 1 hour at a 5 s interval
    std::size_t top_k{16};
    double min_span_s{10.0};  // trends need samples spanning at least this long
};

/// Thread-compatible (callers serialise access; Engine holds a mutex).
class StateHistory {
public:
    explicit StateHistory(HistoryConfig cfg = {}) : cfg_(cfg) {}
    /// A history holding `pts` (oldest first), trimmed to the newest `capacity`.
    StateHistory(HistoryConfig cfg, std::deque<HistoryPoint> pts) : cfg_(cfg), points_(std::move(pts)) {
        while (points_.size() > cfg_.capacity) points_.pop_front();
    }

    /// Records the parts of `s` that history tracks. States without CPU or
    /// memory data are ignored. `pinned` processes (e.g. the conversation's
    /// subject) are kept even if they are not in the top-k.
    /// `seq` is the agent snapshot's sequence number (0 for none).
    void record(const SystemState& s, const std::vector<std::pair<Pid, std::optional<std::uint64_t>>>& pinned = {},
                std::uint64_t seq = 0);

    [[nodiscard]] const std::deque<HistoryPoint>& points() const noexcept { return points_; }
    [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }
    [[nodiscard]] const HistoryConfig& config() const noexcept { return cfg_; }
    /// Approximate heap + inline bytes held (for overhead reporting).
    [[nodiscard]] std::size_t approx_bytes() const;
    void clear() { points_.clear(); }
    /// Applies new bounds to the points already held: keeps the newest
    /// `capacity` points and, if `top_k` shrank, cuts each point to its top_k
    /// processes by CPU and by memory (pinned extras are dropped). Afterwards
    /// every point respects the new bounds and the memory of what was dropped
    /// is released.
    void reconfigure(HistoryConfig cfg);

private:
    HistoryConfig cfg_;
    std::deque<HistoryPoint> points_;
};

/// A stretch of a window in which the history holds no point although one
/// was expected.
struct HistoryGap {
    enum class Kind : std::uint8_t {
        /// Consecutive points further apart than the threshold on the
        /// monotonic clock: the agent was stopped, a collection overran, or
        /// collections failed.
        collection_gap,
        /// Consecutive points close together on the monotonic clock but
        /// further apart on the wall clock: the machine was suspended (the
        /// monotonic clock does not advance during suspend) or the wall
        /// clock was changed.
        suspended_or_clock_change,
        /// Between the start of the requested window and the first point:
        /// the history does not reach back that far.
        before_first_point,
        /// Between the last point and the time of the answer.
        after_last_point,
    };
    Kind kind{Kind::collection_gap};
    WallTime from{}, to{};
    double seconds{0};
};
std::string_view to_string(HistoryGap::Kind k) noexcept;

/// What a window of the history actually holds, compared with what was
/// asked for. Nothing is interpolated: time not covered by consecutive
/// points closer than `gap_threshold_s` counts as unobserved.
struct HistoryCoverage {
    std::optional<double> requested_s;    // the window asked for (nullopt: all of the history)
    WallTime evaluated_at{};              // "now" for the window
    std::optional<WallTime> window_start; // evaluated_at - requested_s
    std::size_t points{0};                // points in the window
    std::optional<WallTime> first, last;  // first and last point in the window
    std::uint64_t first_seq{0}, last_seq{0};
    double observed_span_s{0};            // last - first (monotonic)
    double gap_threshold_s{0};
    std::vector<HistoryGap> gaps;         // in time order, including before/after the points
    double unobserved_s{0};               // total of `gaps`
    /// Covered share of the reference span: the requested window, or (with
    /// no window) the span from the first point to `evaluated_at`. nullopt
    /// when there is no point and no window.
    std::optional<double> coverage;
    std::size_t history_points{0};        // all points held
    std::size_t capacity{0};
    std::optional<WallTime> oldest;       // the oldest point held
};

/// Minimum coverage for a temporal answer to be complete; below it the
/// answer is partial (docs/API.md, "Historical diagnosis").
inline constexpr double kMinHistoryCoverage = 0.8;

/// The coverage of the window [now - window_s, now] (all of the history
/// when window_s is nullopt), selecting points as the engine does
/// (now - point.at <= window_s). O(points).
HistoryCoverage history_coverage(const StateHistory& h, SteadyTime now, WallTime now_wall,
                                 std::optional<double> window_s, double gap_threshold_s);

/// One numeric time series: (seconds since the first point, value).
using Series = std::vector<std::pair<double, double>>;

enum class TrendDirection : std::uint8_t { insufficient_data, increasing, decreasing, stable };
std::string_view to_string(TrendDirection d) noexcept;

struct TrendResult {
    TrendDirection direction{TrendDirection::insufficient_data};
    std::size_t n{0};
    double span_s{0};
    double first{0}, last{0}, min{0}, max{0};
    double early_mean{0}, late_mean{0};  // first and last third of the window
    double slope_per_min{0};             // ordinary least squares
    double mann_kendall_z{0};            // monotonic-trend statistic
    std::string why;                     // reason for insufficient_data
};

struct TrendRule {
    std::size_t min_points{4};
    double min_span_s{10.0};
    /// Smallest change between the early and late thirds that counts as a
    /// change, in the series' units (e.g. 5 percentage points of CPU).
    double min_effect{5.0};
    double z_threshold{1.96};
};

/// Direction is `increasing`/`decreasing` only when both hold: the
/// Mann-Kendall test rejects "no monotonic trend" (|z| > threshold) and the
/// late-third mean differs from the early-third mean by at least min_effect.
/// Caveat: consecutive samples are autocorrelated, which inflates |z|; the
/// effect-size floor is what keeps noise from being reported as a trend.
TrendResult analyse_trend(const Series& s, const TrendRule& rule = {});

}  // namespace osie
