// SPDX-License-Identifier: Apache-2.0
#include "osie/api/evidence.hpp"

#include <algorithm>

namespace osie::api {

thread_local std::uint64_t RefusingOs::thread_calls_ = 0;

std::string derive_cgroup_rates(SystemState& cur, const SystemState* prev) {
    if (cur.cgroups.empty()) return {};
    if (prev == nullptr) return "throttling rates need two agent collections; only one exists so far";
    const double dt = std::chrono::duration<double>(cur.taken_at - prev->taken_at).count();
    if (dt <= 0) return "throttling rates unavailable: collections out of order";
    std::size_t matched = 0;
    for (auto& v : cur.cgroups) {
        const auto it = std::find_if(prev->cgroups.begin(), prev->cgroups.end(),
                                     [&](const CgroupView& p) { return p.info.path == v.info.path; });
        if (it == prev->cgroups.end()) {
            v.window_note = "not in the previous collection: no rate";
            continue;
        }
        CgroupView w = *it;
        derive_cgroup_window(w, v.info, dt);
        v.window_s = w.window_s;
        v.window_throttled_ratio = w.window_throttled_ratio;
        v.window_usage_cpus = w.window_usage_cpus;
        v.window_throttled_cpus = w.window_throttled_cpus;
        if (!w.window_note.empty()) v.window_note = w.window_note;
        ++matched;
    }
    return "throttling rates over the " + std::to_string(static_cast<long>(dt + 0.5)) + " s between the agent's last two "
           "collections (" + std::to_string(matched) + " of " + std::to_string(cur.cgroups.size()) + " cgroups in both)";
}

std::shared_ptr<const SystemState> AgentEvidence::state(const std::vector<ObservationKind>& kinds, bool cgroup_focus,
                                                        const CancellationToken& /*cancel*/, bool& reused) {
    reused = true;  // never sampled for this question
    if (kinds.empty()) return nullptr;  // as the live source: nothing to observe
    r_.consulted = true;
    const auto [cur, prev] = rt_.snapshots();
    if (!cur || !cur->state) {
        r_.no_snapshot = true;
        return nullptr;
    }
    r_.seq = cur->seq;
    r_.collected_at = cur->wall;
    const double age = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cur->mono).count();
    r_.age_ms = age;
    if (age > static_cast<double>(o_.max_age.count())) {
        r_.stale = true;
        return nullptr;
    }
    for (auto k : kinds)
        if (!cur->state->has(k) && std::find(r_.missing.begin(), r_.missing.end(), k) == r_.missing.end())
            r_.missing.push_back(k);
    if (!cgroup_focus && !o_.redact_for) return used_ = cur->state;  // shared, immutable
    auto copy = std::make_shared<SystemState>(*cur->state);
    if (cgroup_focus) r_.cgroup_note = derive_cgroup_rates(*copy, prev && prev->state ? prev->state.get() : nullptr);
    if (o_.redact_for) redact(*copy, *o_.redact_for, o_.visibility);
    return used_ = std::move(copy);
}

const StateHistory& AgentEvidence::history() {
    if (!hist_) {
        hist_ = rt_.history_copy();
        // Under hidepid a non-full peer sees no other user's processes; history
        // points do not record owners, so their process lists are dropped.
        if (o_.redact_for && o_.visibility == ProcVisibility::own_only) {
            std::deque<HistoryPoint> pts(hist_->points().begin(), hist_->points().end());
            for (auto& p : pts) p.processes.clear();
            hist_ = StateHistory(hist_->config(), std::move(pts));
        }
    }
    return *hist_;
}

}  // namespace osie::api
