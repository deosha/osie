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
    void record(const SystemState& s, const std::vector<std::pair<Pid, std::optional<std::uint64_t>>>& pinned = {});

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
