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
    std::string notes;
    for (auto& v : cur.cgroups) {
        const auto it = std::find_if(prev->cgroups.begin(), prev->cgroups.end(),
                                     [&](const CgroupView& p) { return p.info.path == v.info.path; });
        if (it == prev->cgroups.end()) {
            v.window_note = "not in the previous collection: no rate";
            continue;
        }
        CgroupView w = *it;
        // The rates span the two reads of cpu.stat, one in each collection.
        const bool timed = it->read_mono != SteadyTime{} && v.read_mono != SteadyTime{} && v.read_mono > it->read_mono;
        derive_cgroup_window(w, v.info, timed ? std::chrono::duration<double>(v.read_mono - it->read_mono).count() : dt);
        v.window_s = w.window_s;
        v.window_throttled_ratio = w.window_throttled_ratio;
        v.window_usage_cpus = w.window_usage_cpus;
        v.window_throttled_cpus = w.window_throttled_cpus;
        v.window_span.reset();
        v.window_across_collections = true;
        if (v.window_s) {
            v.window_span = timed ? DerivationWindow{it->read_wall, v.read_wall, it->read_mono, v.read_mono}
                                  : DerivationWindow{{}, {}, prev->taken_at, cur.taken_at};
        }
        if (!w.window_note.empty()) {
            v.window_note = w.window_note;
            notes += "; " + v.info.path + ": " + w.window_note;  // e.g. counters reset, quota changed
        }
        ++matched;
    }
    char span[32];
    std::snprintf(span, sizeof span, dt < 10 ? "%.1f" : "%.0f", dt);
    return std::string("throttling rates over the ") + span + " s between the agent's last two collections (" +
           std::to_string(matched) + " of " + std::to_string(cur.cgroups.size()) + " cgroups in both)" + notes;
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
    r_.checked_at = std::chrono::steady_clock::now();
    const double age = std::chrono::duration<double, std::milli>(r_.checked_at - cur->mono).count();
    r_.age_ms = age;
    if (age > static_cast<double>(o_.max_age.count())) {
        r_.stale = true;
        return nullptr;
    }
    for (auto k : kinds) {
        if (cur->state->has(k) || std::find(r_.missing.begin(), r_.missing.end(), k) != r_.missing.end()) continue;
        // No cgroups in the snapshot is not a missing observation unless reading
        // them failed: the busiest processes may simply have none (or have exited).
        if (k == ObservationKind::cgroup) {
            const auto* why = cur->state->why_unavailable(k);
            if (why == nullptr || why->why.code == Errc::not_found || why->why.code == Errc::process_exited) continue;
        }
        r_.missing.push_back(k);
    }
    if (!cgroup_focus && !o_.redact_for) return used_ = cur->state;  // shared, immutable
    auto copy = std::make_shared<SystemState>(*cur->state);
    // Redacted first, so that nothing derived below (the cgroup note and its
    // per-cgroup details) can name what the peer may not see.
    if (o_.redact_for) redact(*copy, *o_.redact_for, o_.visibility);
    if (cgroup_focus) {
        r_.cgroup_note = derive_cgroup_rates(*copy, prev && prev->state ? prev->state.get() : nullptr);
        if (prev && prev->state && !copy->cgroups.empty()) r_.prev_seq = prev->seq;
    }
    return used_ = std::move(copy);
}

namespace {

bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

template <class T>
void from_observation(ItemProvenance& ip, std::string_view kind, const std::optional<Observation<T>>& o) {
    if (!o) return;
    ip.observation = kind;
    ip.observation_validity = o->prov.validity;
    ip.sampled_at = o->prov.wall;
    ip.sampled_mono = o->prov.mono;
    if (o->prov.validity == Validity::partial) ip.notes.emplace_back("observation_partial");
}

void process_list_notes(ItemProvenance& ip, const SystemState& s) {
    if (s.processes_partial > 0 || s.processes_vanished > 0) ip.notes.emplace_back("process_list_incomplete");
}

}  // namespace

ItemProvenance provenance_of(const reason::Evidence& e, const SystemState& s, std::optional<Pid> subject,
                             std::optional<std::uint64_t> prev_seq) {
    ItemProvenance ip;
    const std::string_view id = e.id;
    const std::string_view head = id.substr(0, id.find(':'));
    const bool derived = e.validity == Validity::derived;
    if (head == "cgroup_throttled" || head == "proc_cgroup_throttled") {
        const CgroupView* cg = nullptr;
        std::string path;
        if (head == "cgroup_throttled" && id.size() > head.size()) path = std::string(id.substr(head.size() + 1));
        if (head == "proc_cgroup_throttled" && subject)
            if (const auto* p = s.find_pid(*subject); p != nullptr && p->info.cgroup) path = *p->info.cgroup;
        for (const auto& c : s.cgroups)
            if (!path.empty() && c.info.path == path) cg = &c;
        ip.observation = "cgroup";
        if (cg == nullptr) return ip;
        if (cg->read_mono != SteadyTime{}) {
            ip.sampled_at = cg->read_wall;
            ip.sampled_mono = cg->read_mono;
        }
        const bool windowed = derived && cg->window_throttled_ratio.has_value();
        ip.observation_validity = windowed ? Validity::derived : Validity::valid;
        if (windowed) {
            ip.window = cg->window_span;
            if (cg->window_across_collections) {
                ip.window_from_seq = prev_seq;
                ip.notes.emplace_back("between_collections");
            }
        } else {
            ip.notes.emplace_back("cumulative_not_rate");
        }
        const std::string& wn = cg->window_note;
        if (wn.find("backwards") != std::string::npos) ip.notes.emplace_back("counter_reset");
        if (wn.find("quota changed") != std::string::npos) ip.notes.emplace_back("quota_changed");
        if (!windowed && wn.find("not in the previous") != std::string::npos) ip.notes.emplace_back("no_baseline");
        return ip;
    }
    if (head == "cpu_unattributed_cores" || head == "children_cpu" || starts(head, "proc_")) {
        from_observation(ip, "process_list", s.processes);
        if (derived && s.process_window) ip.window = s.process_window;
        process_list_notes(ip, s);
        return ip;
    }
    if (head == "cpu_clusters") {
        from_observation(ip, "cpu_topology", s.topology);
        return ip;
    }
    if (head == "load_1m") {
        from_observation(ip, "load_average", s.load);
        return ip;
    }
    if (head == "runnable_tasks") {
        // Read with the second CPU sample, as the load average is.
        if (s.load) from_observation(ip, "cpu_times", s.load);
        else from_observation(ip, "cpu_times", s.cpu);
        if (ip.observation_validity == Validity::derived) ip.observation_validity = Validity::valid;
        return ip;
    }
    if (starts(head, "cpu_")) {
        from_observation(ip, "cpu_times", s.cpu);
        if (derived && s.cpu_window) ip.window = s.cpu_window;
        return ip;
    }
    if (head == "thermal_state") {
        from_observation(ip, "thermal_state", s.power);
        return ip;
    }
    if (starts(head, "psi_")) {
        from_observation(ip, "pressure_stall", s.psi);
        return ip;
    }
    if (head == "swapouts_per_s" || head == "pageouts_per_s" || head == "compressions_per_s" ||
        head == "swapins_per_s" || head == "major_faults_per_s") {
        from_observation(ip, "memory", s.memory_rates);
        if (s.memory_rates_window) ip.window = s.memory_rates_window;
        return ip;
    }
    if (starts(head, "mem_") || head == "swap_used") {
        from_observation(ip, "memory", s.memory);
        return ip;
    }
    if (head == "fs_full") {
        from_observation(ip, "filesystems", s.filesystems);
        return ip;
    }
    return ip;
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
