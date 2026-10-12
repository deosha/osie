// SPDX-License-Identifier: Apache-2.0
#include "osie/state/state.hpp"

#include <algorithm>
#include <cctype>
#include <thread>
#include <unordered_map>

namespace osie {
namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

double pct(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

bool wants(const SampleRequest& r, ObservationKind k) {
    return std::find(r.kinds.begin(), r.kinds.end(), k) != r.kinds.end();
}

/// Identity key for matching a process across two samples. Including the
/// start time prevents a reused PID from being treated as the same process.
std::uint64_t identity(const ProcessInfo& p) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.pid)) << 32U) ^
           (p.start_time_us.value_or(0) & 0xFFFFFFFFULL);
}

}  // namespace

std::optional<double> counter_rate(std::optional<std::uint64_t> a, std::optional<std::uint64_t> b, double seconds) {
    if (!a || !b || seconds <= 0 || *b < *a) return std::nullopt;
    return static_cast<double>(*b - *a) / seconds;
}

std::optional<CpuUtilization> derive_cpu_utilization(const CpuSample& a, const CpuSample& b) {
    const auto& x = a.aggregate;
    const auto& y = b.aggregate;
    if (y.total() <= x.total() || y.user < x.user || y.system < x.system || y.idle < x.idle || y.nice < x.nice ||
        y.iowait < x.iowait || y.steal < x.steal || y.irq < x.irq || y.softirq < x.softirq) {
        return std::nullopt;
    }
    const std::uint64_t dt = y.total() - x.total();
    CpuUtilization u;
    u.user_pct = pct(y.user - x.user, dt);
    u.system_pct = pct((y.system - x.system) + (y.irq - x.irq) + (y.softirq - x.softirq), dt);
    u.nice_pct = pct(y.nice - x.nice, dt);
    u.idle_pct = pct(y.idle - x.idle, dt);
    if (b.has_iowait) u.iowait_pct = pct(y.iowait - x.iowait, dt);
    if (b.has_steal) u.steal_pct = pct(y.steal - x.steal, dt);
    if (a.per_cpu.size() == b.per_cpu.size()) {  // topology change => skip per-CPU
        u.per_cpu_busy_pct.reserve(b.per_cpu.size());
        for (std::size_t i = 0; i < b.per_cpu.size(); ++i) {
            const auto& p = a.per_cpu[i];
            const auto& q = b.per_cpu[i];
            if (q.total() < p.total() || q.busy() < p.busy()) {
                u.per_cpu_busy_pct.push_back(0.0);
                continue;
            }
            u.per_cpu_busy_pct.push_back(pct(q.busy() - p.busy(), q.total() - p.total()));
        }
    }
    return u;
}

MemoryRates derive_memory_rates(const MemoryMetrics& a, const MemoryMetrics& b, double s) {
    MemoryRates r;
    r.pageins_per_s = counter_rate(a.pageins, b.pageins, s);
    r.pageouts_per_s = counter_rate(a.pageouts, b.pageouts, s);
    r.swapins_per_s = counter_rate(a.swapins, b.swapins, s);
    r.swapouts_per_s = counter_rate(a.swapouts, b.swapouts, s);
    r.compressions_per_s = counter_rate(a.compressions, b.compressions, s);
    r.decompressions_per_s = counter_rate(a.decompressions, b.decompressions, s);
    r.faults_per_s = counter_rate(a.faults, b.faults, s);
    r.major_faults_per_s = counter_rate(a.major_faults, b.major_faults, s);
    return r;
}

std::vector<ProcessView> derive_process_views(const ProcessSnapshot& a, const ProcessSnapshot& b, double seconds) {
    std::unordered_map<std::uint64_t, const ProcessInfo*> before;
    before.reserve(a.processes.size());
    for (const auto& p : a.processes) before.emplace(identity(p), &p);

    std::vector<ProcessView> out;
    out.reserve(b.processes.size());
    for (const auto& p : b.processes) {
        ProcessView v;
        v.info = p;
        auto it = before.find(identity(p));
        if (it == before.end()) {
            v.new_in_interval = true;
        } else if (seconds > 0) {
            const auto t0 = it->second->cpu_total_ns();
            const auto t1 = p.cpu_total_ns();
            if (t0 && t1 && *t1 >= *t0) {
                v.cpu_pct = 100.0 * static_cast<double>(*t1 - *t0) / (seconds * 1e9);
            }
        }
        out.push_back(std::move(v));
    }
    return out;
}

// ---- SystemState queries ------------------------------------------------

std::vector<const ProcessView*> SystemState::top_by_cpu(std::size_t n) const {
    std::vector<const ProcessView*> v;
    if (!processes) return v;
    for (const auto& p : processes->value)
        if (p.cpu_pct) v.push_back(&p);
    const std::size_t k = std::min(n, v.size());
    std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end(),
                      [](auto* x, auto* y) { return *x->cpu_pct > *y->cpu_pct; });
    v.resize(k);
    return v;
}

std::vector<const ProcessView*> SystemState::top_by_memory(std::size_t n) const {
    std::vector<const ProcessView*> v;
    if (!processes) return v;
    for (const auto& p : processes->value)
        if (p.info.memory_bytes()) v.push_back(&p);
    const std::size_t k = std::min(n, v.size());
    std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end(),
                      [](auto* x, auto* y) { return *x->info.memory_bytes() > *y->info.memory_bytes(); });
    v.resize(k);
    return v;
}

const ProcessView* SystemState::find_pid(Pid pid) const {
    if (!processes) return nullptr;
    for (const auto& p : processes->value)
        if (p.info.pid == pid) return &p;
    return nullptr;
}

std::vector<const ProcessView*> SystemState::find_by_name(std::string_view name) const {
    std::vector<const ProcessView*> exact, partial;
    if (!processes || name.empty()) return exact;
    const std::string needle = lower(name);
    for (const auto& p : processes->value) {
        const std::string n = lower(p.info.name);
        if (n == needle) {
            exact.push_back(&p);
        } else if (n.find(needle) != std::string::npos || lower(p.info.path).find(needle) != std::string::npos) {
            partial.push_back(&p);
        }
    }
    auto by_mem = [](auto* x, auto* y) { return x->info.memory_bytes().value_or(0) > y->info.memory_bytes().value_or(0); };
    std::sort(exact.begin(), exact.end(), by_mem);
    std::sort(partial.begin(), partial.end(), by_mem);
    exact.insert(exact.end(), partial.begin(), partial.end());
    return exact;
}

std::vector<const ProcessView*> SystemState::children_of(Pid pid) const {
    std::vector<const ProcessView*> v;
    if (!processes) return v;
    for (const auto& p : processes->value)
        if (p.info.ppid == pid && p.info.pid != pid) v.push_back(&p);
    return v;
}

bool SystemState::has(ObservationKind k) const {
    switch (k) {
        case ObservationKind::system_info: return system.has_value();
        case ObservationKind::cpu_topology: return topology.has_value();
        case ObservationKind::cpu_times: return cpu.has_value();
        case ObservationKind::load_average: return load.has_value();
        case ObservationKind::process_list:
        case ObservationKind::process_detail: return processes.has_value();
        case ObservationKind::memory:
        case ObservationKind::swap: return memory.has_value();
        case ObservationKind::memory_pressure_level: return memory && memory->value.pressure_level.has_value();
        case ObservationKind::filesystems: return filesystems.has_value();
        case ObservationKind::network_interfaces: return net.has_value();
        case ObservationKind::thermal_state: return power.has_value();
        case ObservationKind::pressure_stall: return psi.has_value();
        case ObservationKind::cgroup: return !cgroups.empty();
        case ObservationKind::count_: break;
    }
    return false;
}

const Unavailable* SystemState::why_unavailable(ObservationKind k) const {
    for (const auto& u : unavailable)
        if (u.kind == k) return &u;
    return nullptr;
}

std::uint32_t SystemState::logical_cpus() const {
    if (topology) return topology->value.logical_cpus;
    if (cpu && !cpu->value.per_cpu_busy_pct.empty()) return static_cast<std::uint32_t>(cpu->value.per_cpu_busy_pct.size());
    return 0;
}

// ---- sampler ------------------------------------------------------------

SystemState StateSampler::sample(const SampleRequest& req, const CancellationToken& cancel) {
    using K = ObservationKind;
    SystemState st;
    auto note_fail = [&st](K k, const Error& e) { st.unavailable.push_back(Unavailable{k, e}); };

    // One-shot observations (no deltas needed).
    if (wants(req, K::system_info)) {
        if (auto r = broker_.system_info()) st.system = std::move(r).value(); else note_fail(K::system_info, r.error());
    }
    if (wants(req, K::cpu_topology) || wants(req, K::cpu_times)) {
        if (auto r = broker_.cpu_topology()) st.topology = std::move(r).value(); else note_fail(K::cpu_topology, r.error());
    }
    if (wants(req, K::thermal_state)) {
        if (auto r = broker_.power_state()) st.power = std::move(r).value(); else note_fail(K::thermal_state, r.error());
    }
    if (wants(req, K::pressure_stall)) {
        if (auto r = broker_.pressure()) st.psi = std::move(r).value(); else note_fail(K::pressure_stall, r.error());
    }
    if (wants(req, K::filesystems)) {
        if (auto r = broker_.filesystems()) st.filesystems = std::move(r).value(); else note_fail(K::filesystems, r.error());
    }

    const bool need_cpu = wants(req, K::cpu_times) || wants(req, K::load_average);
    const bool need_mem = wants(req, K::memory) || wants(req, K::swap) || wants(req, K::memory_pressure_level);
    const bool need_proc = wants(req, K::process_list) || wants(req, K::process_detail);
    const bool need_net = wants(req, K::network_interfaces);
    const bool need_delta = need_cpu || need_mem || need_proc || need_net;

    std::optional<Observation<CpuSample>> c0, c1;
    std::optional<Observation<MemoryMetrics>> m0, m1;
    std::optional<Observation<ProcessSnapshot>> p0, p1;
    std::optional<Observation<std::vector<NetInterface>>> n0, n1;

    auto take = [&](auto& c, auto& m, auto& p, auto& n, bool record_errors) {
        if (need_cpu) {
            if (auto r = broker_.cpu_sample()) c = std::move(r).value();
            else if (record_errors) note_fail(K::cpu_times, r.error());
        }
        if (need_mem) {
            if (auto r = broker_.memory()) m = std::move(r).value();
            else if (record_errors) note_fail(K::memory, r.error());
        }
        if (need_proc) {
            if (auto r = broker_.processes(ProcessQuery{}, cancel)) p = std::move(r).value();
            else if (record_errors) note_fail(K::process_list, r.error());
        }
        if (need_net) {
            if (auto r = broker_.network_interfaces()) n = std::move(r).value();
            else if (record_errors) note_fail(K::network_interfaces, r.error());
        }
    };

    if (need_delta) {
        take(c0, m0, p0, n0, false);
        // Sleep in slices so cancellation stays responsive.
        const auto deadline = std::chrono::steady_clock::now() + req.interval;
        while (std::chrono::steady_clock::now() < deadline && !cancel.cancelled()) {
            std::this_thread::sleep_for(std::min<Millis>(Millis{20}, req.interval));
        }
        if (cancel.cancelled()) {
            note_fail(K::cpu_times, make_error(Errc::cancelled, "sampling cancelled"));
            st.taken_at = std::chrono::steady_clock::now();
            return st;
        }
        take(c1, m1, p1, n1, true);
    }

    auto secs = [](const Provenance& a, const Provenance& b) {
        return std::chrono::duration<double>(b.mono - a.mono).count();
    };

    if (c0 && c1) {
        if (auto u = derive_cpu_utilization(c0->value, c1->value)) {
            Provenance pv = c1->prov;
            pv.validity = Validity::derived;
            st.cpu = Observation<CpuUtilization>{*u, pv};
        } else {
            note_fail(K::cpu_times, make_error(Errc::unavailable, "CPU counters went backwards (reset or hotplug)"));
        }
        if (c1->value.load) st.load = Observation<LoadAverage>{*c1->value.load, c1->prov};
        else if (wants(req, K::load_average)) note_fail(K::load_average, make_error(Errc::unavailable, "no load average"));
        st.runnable_tasks = c1->value.runnable_tasks;
        const double s = secs(c0->prov, c1->prov);
        if (auto r = counter_rate(c0->value.context_switches_total, c1->value.context_switches_total, s)) {
            st.context_switches_per_s = *r;
        }
    }
    if (m0 && m1) {
        Provenance pv = m1->prov;
        pv.validity = Validity::derived;
        st.memory_rates = Observation<MemoryRates>{derive_memory_rates(m0->value, m1->value, secs(m0->prov, m1->prov)), pv};
    }
    if (m1) st.memory = std::move(m1);
    if (p1) {
        const double s = p0 ? secs(p0->prov, p1->prov) : 0.0;
        auto views = p0 ? derive_process_views(p0->value, p1->value, s)
                        : derive_process_views(ProcessSnapshot{}, p1->value, 0.0);
        st.processes_partial = p1->value.partial_count;
        st.processes_vanished = p1->value.vanished_count + (p0 ? p0->value.vanished_count : 0);
        Provenance pv = p1->prov;
        if (p0 && pv.validity == Validity::valid) pv.validity = Validity::derived;
        st.processes = Observation<std::vector<ProcessView>>{std::move(views), pv};
    }
    if (n0 && n1) {
        const double s = secs(n0->prov, n1->prov);
        for (const auto& b : n1->value) {
            for (const auto& a : n0->value) {
                if (a.name != b.name) continue;
                st.net_rates.push_back(NetRates{b.name, counter_rate(a.rx_bytes, b.rx_bytes, s),
                                                counter_rate(a.tx_bytes, b.tx_bytes, s)});
            }
        }
    }
    if (n1) st.net = std::move(n1);

    // Linux: cgroup view of the busiest processes (throttling evidence).
    if (wants(req, K::cgroup) && st.processes) {
        std::vector<std::string> seen;
        for (const auto* pv : st.top_by_cpu(req.cgroup_top_n)) {
            std::string path;
            if (pv->info.cgroup) {
                path = *pv->info.cgroup;
            } else if (auto r = broker_.cgroup_of(pv->info.pid)) {
                path = *r;
            } else {
                if (st.why_unavailable(K::cgroup) == nullptr) note_fail(K::cgroup, r.error());
                continue;
            }
            if (std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
            seen.push_back(path);
            if (auto r = broker_.cgroup(path)) {
                CgroupView cv{r->value, std::nullopt};
                if (cv.info.nr_periods && cv.info.nr_throttled && *cv.info.nr_periods > 0) {
                    cv.lifetime_throttled_ratio =
                        static_cast<double>(*cv.info.nr_throttled) / static_cast<double>(*cv.info.nr_periods);
                }
                st.cgroups.push_back(std::move(cv));
            }
        }
    }

    st.interval = req.interval;
    st.taken_at = std::chrono::steady_clock::now();
    return st;
}

void StateCache::store(std::shared_ptr<const SystemState> s) {
    std::unique_lock lk(mu_);
    state_ = std::move(s);
}

std::shared_ptr<const SystemState> StateCache::load() const {
    std::shared_lock lk(mu_);
    return state_;
}

std::shared_ptr<const SystemState> StateCache::fresh(const std::vector<ObservationKind>& kinds, Millis budget) const {
    std::shared_lock lk(mu_);
    if (!state_) return nullptr;
    if (std::chrono::steady_clock::now() - state_->taken_at > budget) return nullptr;
    for (auto k : kinds) {
        // An observation that was attempted and failed counts as present (no point re-asking).
        if (!state_->has(k) && state_->why_unavailable(k) == nullptr) return nullptr;
    }
    return state_;
}

}  // namespace osie
