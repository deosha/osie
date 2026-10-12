// SPDX-License-Identifier: Apache-2.0
#include "osie/prediction/features.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace osie::predict {

std::string_view to_string(ActionKind k) noexcept {
    switch (k) {
        case ActionKind::none: return "none";
        case ActionKind::cpu_quota: return "cpu_quota";
        case ActionKind::cpuset: return "cpuset";
        case ActionKind::offered_load: return "offered_load";
    }
    return "?";
}

std::optional<ActionKind> action_from_string(std::string_view s) noexcept {
    for (auto k : {ActionKind::none, ActionKind::cpu_quota, ActionKind::cpuset, ActionKind::offered_load})
        if (to_string(k) == s) return k;
    return std::nullopt;
}

namespace {

std::optional<double> delta(std::optional<std::uint64_t> a, std::optional<std::uint64_t> b) {
    if (!a || !b || *b < *a) return std::nullopt;
    return static_cast<double>(*b - *a);
}

const PressureStall* psi_cpu(const CgroupInfo& c) {
    for (const auto& p : c.pressure)
        if (p.resource == PsiResource::cpu) return &p;
    return nullptr;
}

/// CPUs the cgroup may use regardless of quota.
double cpus_available(const CgroupWindow& w) { return w.cpuset_cpus.value_or(w.host_cpus.value_or(1.0)); }

double capacity_before(const CgroupWindow& w) {
    const double cpus = cpus_available(w);
    return w.quota_cores ? std::min(*w.quota_cores, cpus) : cpus;
}

}  // namespace

Result<CgroupWindow> make_window(const CgroupInfo& a, const CgroupInfo& b, double secs, const CpuSample* ha,
                                 const CpuSample* hb) {
    if (!(secs > 0.0) || !std::isfinite(secs)) return make_error(Errc::invalid_argument, "window length must be > 0");
    CgroupWindow w;
    w.secs = secs;
    const auto usage = delta(a.usage_usec, b.usage_usec);
    if (!usage) return make_error(Errc::unavailable, "cpu.stat usage missing or went backwards");
    w.usage_cores = *usage / (secs * 1e6);
    if (auto periods = delta(a.nr_periods, b.nr_periods)) {
        auto thr = delta(a.nr_throttled, b.nr_throttled);
        // No periods elapsed (e.g. an unlimited quota): nothing could be throttled.
        w.throttled_ratio = *periods > 0 ? std::clamp(thr.value_or(0.0) / *periods, 0.0, 1.0) : 0.0;
    }
    // Not clamped to 1: on the kernels measured (6.10) throttled_usec sums
    // throttled time across CPUs, so it can exceed wall time.
    if (auto t = delta(a.throttled_usec, b.throttled_usec)) w.throttled_frac = std::max(0.0, *t / (secs * 1e6));
    if (const auto *pa = psi_cpu(a), *pb = psi_cpu(b); pa && pb) {
        if (pb->some.total_us >= pa->some.total_us)
            w.psi_some = std::clamp(static_cast<double>(pb->some.total_us - pa->some.total_us) / (secs * 1e6), 0.0, 1.0);
        if (pa->full && pb->full && pb->full->total_us >= pa->full->total_us)
            w.psi_full = std::clamp(static_cast<double>(pb->full->total_us - pa->full->total_us) / (secs * 1e6), 0.0, 1.0);
    }
    if (b.cpu_quota_us && b.cpu_period_us && *b.cpu_period_us > 0)
        w.quota_cores = static_cast<double>(*b.cpu_quota_us) / static_cast<double>(*b.cpu_period_us);
    if (b.cpuset_cpus) w.cpuset_cpus = *b.cpuset_cpus;
    if (b.pids_current) w.pids = static_cast<double>(*b.pids_current);
    if (ha != nullptr && hb != nullptr) {
        const auto& x = ha->aggregate;
        const auto& y = hb->aggregate;
        if (y.total() > x.total() && y.busy() >= x.busy())
            w.host_busy_frac = static_cast<double>(y.busy() - x.busy()) / static_cast<double>(y.total() - x.total());
        if (!hb->per_cpu.empty()) w.host_cpus = static_cast<double>(hb->per_cpu.size());
    }
    return w;
}

double demand_estimate(const CgroupWindow& w) {
    // Runnable CPU time = time used + time spent throttled (both summed over
    // CPUs). Measured on Linux 6.10: 3 busy threads under a 1-core quota give
    // usage 1.0 + throttled 2.0 cores (docs/WORLD_MODEL_EXPERIMENTS.md).
    return std::min(w.usage_cores + w.throttled_frac.value_or(0.0), cpus_available(w));
}

double capacity_after(const CgroupWindow& w, const Intervention& a) {
    switch (a.kind) {
        case ActionKind::cpu_quota: return std::min(a.proposed, cpus_available(w));
        case ActionKind::cpuset: return w.quota_cores ? std::min(*w.quota_cores, a.proposed) : a.proposed;
        case ActionKind::none:
        case ActionKind::offered_load: break;
    }
    return capacity_before(w);
}

Result<void> validate(const Intervention& a, const CgroupWindow& cur) {
    auto bad = [](std::string m) { return make_error(Errc::invalid_argument, std::move(m)); };
    if (!std::isfinite(a.previous) || !std::isfinite(a.proposed)) return bad("intervention values must be finite");
    const double host = cur.host_cpus.value_or(1024.0);
    switch (a.kind) {
        case ActionKind::none: return {};
        case ActionKind::cpu_quota:
            if (!(a.proposed >= 0.01)) return bad("a CPU quota must be at least 0.01 cores");
            if (a.proposed > host) return bad("a CPU quota above the host's " + std::to_string(static_cast<int>(host)) + " CPUs has no effect");
            return {};
        case ActionKind::cpuset:
            if (a.proposed < 1 || a.proposed > host || std::floor(a.proposed) != a.proposed)
                return bad("a cpuset size must be a whole number of CPUs between 1 and the host's CPU count");
            return {};
        case ActionKind::offered_load:
            if (!(a.previous > 0) || !(a.proposed > 0) || a.proposed / a.previous > 10 || a.previous / a.proposed > 10)
                return bad("offered-load factors must be positive and within 10x");
            return {};
    }
    return bad("unknown intervention");
}

const std::vector<std::string>& feature_names() {
    static const std::vector<std::string> names = {
        // generic telemetry (window means unless noted) and raw action values
        "g_usage", "g_usage_last", "g_throttled_ratio", "g_throttled_frac", "g_psi_some", "g_psi_full", "g_quota",
        "g_quota_unlimited", "g_cpuset", "g_pids", "g_host_busy", "g_host_cpus", "g_miss_throttle", "g_miss_psi",
        "g_miss_host", "g_act_none", "g_act_quota", "g_act_cpuset", "g_act_load", "g_act_previous", "g_act_proposed",
        // mechanism-aware (cgroup v2 CPU bandwidth control)
        "m_cap_before", "m_cap_after", "m_log_cap_ratio", "m_demand", "m_demand_after", "m_util_before",
        "m_pressure_after", "m_headroom_after", "m_mech_usage_after", "m_excess_after", "m_parallel_after",
        // history
        "h_usage_slope", "h_usage_std", "h_throttle_slope", "h_windows"};
    return names;
}

Result<EncodedFeatures> encode(const std::vector<CgroupWindow>& hist, const Intervention& a) {
    if (hist.empty()) return make_error(Errc::invalid_argument, "at least one observation window is required");
    const auto& last = hist.back();
    if (auto v = validate(a, last); !v) return v.error();
    EncodedFeatures e;
    const auto n = static_cast<double>(hist.size());
    auto mean = [&](auto get) {
        double s = 0;
        for (const auto& w : hist) s += get(w);
        return s / n;
    };
    auto any_missing = [&](auto has) { return std::any_of(hist.begin(), hist.end(), [&](const auto& w) { return !has(w); }); };
    const bool miss_thr = any_missing([](const CgroupWindow& w) { return w.throttled_ratio.has_value(); });
    const bool miss_psi = any_missing([](const CgroupWindow& w) { return w.psi_some.has_value(); });
    const bool miss_host = any_missing([](const CgroupWindow& w) { return w.host_busy_frac.has_value(); });
    if (miss_thr) e.missing.emplace_back("throttling statistics (cpu controller not enabled)");
    if (miss_psi) e.missing.emplace_back("cgroup CPU pressure (PSI)");
    if (miss_host) e.missing.emplace_back("host CPU utilisation");

    CgroupWindow avg = last;  // mean window, used for the mechanism features
    avg.usage_cores = mean([](const CgroupWindow& w) { return w.usage_cores; });
    avg.throttled_frac = mean([](const CgroupWindow& w) { return w.throttled_frac.value_or(0.0); });
    const double thr_ratio = mean([](const CgroupWindow& w) { return w.throttled_ratio.value_or(0.0); });
    const double psi_some = mean([](const CgroupWindow& w) { return w.psi_some.value_or(0.0); });
    const double psi_full = mean([](const CgroupWindow& w) { return w.psi_full.value_or(0.0); });
    const double host_busy = mean([](const CgroupWindow& w) { return w.host_busy_frac.value_or(0.0); });

    const double cap_b = capacity_before(avg);
    const double cap_a = capacity_after(avg, a);
    const double demand = demand_estimate(avg);
    const double load_factor = a.kind == ActionKind::offered_load ? a.proposed / a.previous : 1.0;
    const double cpus_after = a.kind == ActionKind::cpuset ? a.proposed : cpus_available(avg);
    const double demand_after = std::min(demand * load_factor, cpus_after);
    const double eps = 1e-3;

    // History: least-squares slope per second over the window sequence.
    auto slope = [&](auto get) {
        if (hist.size() < 2) return 0.0;
        double t = 0, mt = 0, mv = 0;
        std::vector<double> ts;
        for (const auto& w : hist) {
            ts.push_back(t);
            t += w.secs;
        }
        for (std::size_t i = 0; i < hist.size(); ++i) mt += ts[i], mv += get(hist[i]);
        mt /= n;
        mv /= n;
        double sxy = 0, sxx = 0;
        for (std::size_t i = 0; i < hist.size(); ++i) {
            sxy += (ts[i] - mt) * (get(hist[i]) - mv);
            sxx += (ts[i] - mt) * (ts[i] - mt);
        }
        return sxx > 0 ? sxy / sxx : 0.0;
    };
    double var = 0;
    for (const auto& w : hist) var += (w.usage_cores - avg.usage_cores) * (w.usage_cores - avg.usage_cores);

    e.values = {
        avg.usage_cores, last.usage_cores, thr_ratio, *avg.throttled_frac, psi_some, psi_full,
        last.quota_cores.value_or(0.0), last.quota_cores ? 0.0 : 1.0, cpus_available(last), last.pids.value_or(0.0),
        host_busy, last.host_cpus.value_or(0.0), miss_thr ? 1.0 : 0.0, miss_psi ? 1.0 : 0.0, miss_host ? 1.0 : 0.0,
        a.kind == ActionKind::none ? 1.0 : 0.0, a.kind == ActionKind::cpu_quota ? 1.0 : 0.0,
        a.kind == ActionKind::cpuset ? 1.0 : 0.0, a.kind == ActionKind::offered_load ? 1.0 : 0.0,
        a.kind == ActionKind::none ? 0.0 : a.previous, a.kind == ActionKind::none ? 0.0 : a.proposed,
        cap_b, cap_a, std::log((cap_a + eps) / (cap_b + eps)), demand, demand_after, avg.usage_cores / (cap_b + eps),
        demand_after / (cap_a + eps), cap_a - demand_after, std::min(demand_after, cap_a),
        std::max(0.0, demand_after - cap_a) / (cap_a + eps),
        std::min(last.pids.value_or(cpus_after), cpus_after) / (cap_a + eps),
        slope([](const CgroupWindow& w) { return w.usage_cores; }), std::sqrt(var / n),
        slope([](const CgroupWindow& w) { return w.throttled_ratio.value_or(0.0); }), n};
    for (double& v : e.values)
        if (!std::isfinite(v)) return make_error(Errc::internal, "non-finite feature");
    return e;
}


std::optional<double> mechanistic_prior(std::string_view target, const std::vector<double>& e) {
    const auto& names = feature_names();
    if (e.size() != names.size()) return std::nullopt;
    auto f = [&](std::string_view n) {
        return e[static_cast<std::size_t>(std::find(names.begin(), names.end(), n) - names.begin())];
    };
    const double quota_after = f("g_act_quota") > 0 ? f("g_act_proposed")
                               : f("g_quota_unlimited") > 0 ? std::numeric_limits<double>::infinity()
                                                            : f("g_quota");
    const double cpus_after = f("g_act_cpuset") > 0 ? f("g_act_proposed") : f("g_cpuset");
    const double demand = f("m_demand_after");
    const double cap = std::min(quota_after, cpus_after);
    const bool binds = quota_after < std::min(demand, cpus_after);
    if (target == "usage_cores") return std::min(demand, cap);
    if (target == "throttled_cores") return binds ? demand - cap : 0.0;
    if (target == "throttled_ratio") return binds ? 1.0 : 0.0;
    if (target == "psi_some") return f("g_psi_some");
    return std::nullopt;
}

}  // namespace osie::predict
