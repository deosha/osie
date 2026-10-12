// SPDX-License-Identifier: Apache-2.0
#include "osie/state/history.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace osie {

std::string_view to_string(TrendDirection d) noexcept {
    switch (d) {
        case TrendDirection::insufficient_data: return "insufficient_data";
        case TrendDirection::increasing: return "increasing";
        case TrendDirection::decreasing: return "decreasing";
        case TrendDirection::stable: return "stable";
    }
    return "?";
}

void StateHistory::record(const SystemState& s, const std::vector<std::pair<Pid, std::optional<std::uint64_t>>>& pinned) {
    if (!s.cpu && !s.memory && !s.processes) return;
    HistoryPoint p;
    p.at = s.taken_at;
    p.wall = std::chrono::system_clock::now();
    if (s.cpu) p.cpu_busy_pct = s.cpu->value.busy_pct();
    if (s.memory) {
        p.mem_available_bytes = s.memory->value.available_bytes;
        p.mem_total_bytes = s.memory->value.total_bytes;
        p.pressure = s.memory->value.pressure_level;
    }
    if (s.psi) {
        for (const auto& ps : s.psi->value) {
            if (ps.resource == PsiResource::cpu) p.psi_cpu_some10 = ps.some.avg10;
            if (ps.resource == PsiResource::memory) p.psi_mem_some10 = ps.some.avg10;
        }
    }
    if (s.processes) {
        std::vector<const ProcessView*> keep = s.top_by_cpu(cfg_.top_k);
        for (const auto* m : s.top_by_memory(cfg_.top_k))
            if (std::find(keep.begin(), keep.end(), m) == keep.end()) keep.push_back(m);
        for (const auto& [pid, start] : pinned) {
            const auto* v = s.find_pid(pid);
            if (v == nullptr || (start && v->info.start_time_us && *start != *v->info.start_time_us)) continue;
            if (std::find(keep.begin(), keep.end(), v) == keep.end()) keep.push_back(v);
        }
        p.processes.reserve(keep.size());
        for (const auto* v : keep)
            p.processes.push_back(ProcessPoint{v->info.pid, v->info.start_time_us, v->info.name, v->cpu_pct,
                                               v->info.memory_bytes()});
    }
    // Keep points in time order even if a cached state is recorded twice.
    if (!points_.empty() && p.at <= points_.back().at) return;
    points_.push_back(std::move(p));
    while (points_.size() > cfg_.capacity) points_.pop_front();
}

std::size_t StateHistory::approx_bytes() const {
    std::size_t n = sizeof(*this);
    for (const auto& p : points_) {
        n += sizeof(HistoryPoint) + p.processes.capacity() * sizeof(ProcessPoint);
        for (const auto& q : p.processes) n += q.name.capacity();
    }
    return n;
}

TrendResult analyse_trend(const Series& s, const TrendRule& rule) {
    TrendResult r;
    r.n = s.size();
    if (s.empty()) {
        r.why = "no samples";
        return r;
    }
    r.span_s = s.back().first - s.front().first;
    r.first = s.front().second;
    r.last = s.back().second;
    r.min = r.max = r.first;
    for (const auto& [t, v] : s) {
        r.min = std::min(r.min, v);
        r.max = std::max(r.max, v);
    }
    if (s.size() < rule.min_points || r.span_s < rule.min_span_s) {
        r.why = std::to_string(s.size()) + " sample(s) over " + std::to_string(static_cast<long>(std::lround(r.span_s))) +
                " s; at least " + std::to_string(rule.min_points) + " samples over " +
                std::to_string(static_cast<long>(rule.min_span_s)) + " s are needed";
        return r;
    }
    const std::size_t n = s.size();
    const std::size_t third = std::max<std::size_t>(1, n / 3);
    for (std::size_t i = 0; i < third; ++i) r.early_mean += s[i].second;
    for (std::size_t i = n - third; i < n; ++i) r.late_mean += s[i].second;
    r.early_mean /= static_cast<double>(third);
    r.late_mean /= static_cast<double>(third);

    // OLS slope.
    double mt = 0, mv = 0;
    for (const auto& [t, v] : s) mt += t, mv += v;
    mt /= static_cast<double>(n);
    mv /= static_cast<double>(n);
    double sxy = 0, sxx = 0;
    for (const auto& [t, v] : s) sxy += (t - mt) * (v - mv), sxx += (t - mt) * (t - mt);
    r.slope_per_min = sxx > 0 ? 60.0 * sxy / sxx : 0.0;

    // Mann-Kendall S with the no-ties variance (ties only make it conservative).
    long long S = 0;
    for (std::size_t i = 0; i + 1 < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) S += (s[j].second > s[i].second) - (s[j].second < s[i].second);
    const double dn = static_cast<double>(n);
    const double var = dn * (dn - 1) * (2 * dn + 5) / 18.0;
    const double sd = std::sqrt(var);
    r.mann_kendall_z = S > 0 ? (static_cast<double>(S) - 1) / sd : S < 0 ? (static_cast<double>(S) + 1) / sd : 0.0;

    const double effect = r.late_mean - r.early_mean;
    if (r.mann_kendall_z > rule.z_threshold && effect >= rule.min_effect) r.direction = TrendDirection::increasing;
    else if (r.mann_kendall_z < -rule.z_threshold && -effect >= rule.min_effect) r.direction = TrendDirection::decreasing;
    else r.direction = TrendDirection::stable;
    return r;
}

}  // namespace osie
