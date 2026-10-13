// SPDX-License-Identifier: Apache-2.0
#include "osie/reasoning/reasoner.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "osie/architecture/model.hpp"
#include "osie/util/format.hpp"
#include "osie/util/json.hpp"

namespace osie::reason {
namespace ck = arch::concept_key;

std::string_view to_string(FindingKind k) noexcept {
    switch (k) {
        case FindingKind::observation: return "observation";
        case FindingKind::hypothesis: return "hypothesis";
        case FindingKind::correlation: return "correlation";
        case FindingKind::causal_explanation: return "causal_explanation";
        case FindingKind::suggested_investigation: return "suggested_investigation";
        case FindingKind::proposed_optimization: return "proposed_optimization";
    }
    return "?";
}

std::string_view to_string(Certainty c) noexcept {
    switch (c) {
        case Certainty::none: return "none";
        case Certainty::low: return "low";
        case Certainty::limited: return "limited";
        case Certainty::moderate: return "moderate";
        case Certainty::high: return "high";
    }
    return "?";
}

const Evidence* Diagnosis::find_evidence(std::string_view id) const {
    for (const auto& e : evidence)
        if (e.id == id) return &e;
    return nullptr;
}

namespace {

void add_ev(Diagnosis& d, std::string id, std::string text, std::string_view src, std::optional<double> v,
            Validity val = Validity::valid) {
    d.evidence.push_back(Evidence{std::move(id), std::move(text), src, v, val});
}

Finding finding(FindingKind k, std::string code, std::string text, std::vector<std::string> ev, Certainty c) {
    return Finding{k, std::move(code), std::move(text), std::move(ev), c, {}};
}

const PressureStall* psi_of(const SystemState& s, PsiResource r) {
    if (!s.psi) return nullptr;
    for (const auto& p : s.psi->value)
        if (p.resource == r) return &p;
    return nullptr;
}

std::string proc_label(const ProcessInfo& p) { return p.name + " (pid " + std::to_string(p.pid) + ")"; }

void note_unavailable(Diagnosis& d, const SystemState& s, ObservationKind k, std::string_view what) {
    if (const auto* u = s.why_unavailable(k)) {
        d.missing_evidence.push_back(std::string(what) + ": " + u->why.describe());
    }
}

}  // namespace

std::optional<Mechanism> Reasoner::mechanism(std::string_view from, std::string_view to) const {
    const auto a = arch::concept_node(g_, from);
    const auto b = arch::concept_node(g_, to);
    if (a == arch::kNoNode || b == arch::kNoNode) return std::nullopt;
    for (auto eid : g_.out_edges(a)) {
        const auto& e = g_.edge(eid);
        if (e.to != b || !e.platforms.contains(platform_)) continue;
        return Mechanism{g_.node(a).label, std::string(arch::to_string(e.kind)), g_.node(b).label, e.nature,
                         e.confidence, e.mechanism, e.doc_ref};
    }
    return std::nullopt;
}

std::vector<Mechanism> Reasoner::mechanisms_from(std::string_view key, std::size_t depth) const {
    std::vector<Mechanism> out;
    const auto a = arch::concept_node(g_, key);
    if (a == arch::kNoNode) return out;
    for (auto eid : g_.consequences(a, platform_, depth)) {
        const auto& e = g_.edge(eid);
        out.push_back(Mechanism{g_.node(e.from).label, std::string(arch::to_string(e.kind)), g_.node(e.to).label,
                                e.nature, e.confidence, e.mechanism, e.doc_ref});
    }
    return out;
}

// ---------------------------------------------------------------- CPU ----

Diagnosis Reasoner::cpu(const SystemState& s) const {
    Diagnosis d;
    d.topic = "cpu";
    if (!s.cpu) {
        d.conclusion = "insufficient_evidence";
        d.headline = "CPU utilisation could not be observed.";
        d.certainty = Certainty::none;
        note_unavailable(d, s, ObservationKind::cpu_times, "cpu utilisation");
        d.next_step = "retry; check adapter capabilities with ':caps'";
        return d;
    }
    const auto& u = s.cpu->value;
    const auto src = s.cpu->prov.source;
    const double ncpu = std::max<std::uint32_t>(1, s.logical_cpus());
    const double busy = std::clamp(u.busy_pct(), 0.0, 100.0);
    const double busy_cores = busy / 100.0 * ncpu;
    const double window_s = std::chrono::duration<double>(s.interval).count();

    add_ev(d, "cpu_busy_pct",
           "CPU busy " + fmt::pct(busy) + " of " + std::to_string(static_cast<int>(ncpu)) + " logical CPUs (user " +
               fmt::pct(u.user_pct) + ", system " + fmt::pct(u.system_pct) + ") over " + fmt::num(window_s, 2) + " s",
           src, busy, Validity::derived);
    if (!u.per_cpu_busy_pct.empty()) {
        const double mx = *std::max_element(u.per_cpu_busy_pct.begin(), u.per_cpu_busy_pct.end());
        const auto saturated = std::count_if(u.per_cpu_busy_pct.begin(), u.per_cpu_busy_pct.end(),
                                             [](double v) { return v >= 90.0; });
        add_ev(d, "cpu_max_logical_pct",
               "busiest logical CPU " + fmt::pct(mx) + "; " + std::to_string(saturated) + " CPUs >= 90%", src, mx,
               Validity::derived);
    }
    std::optional<double> load_ratio;
    if (s.load) {
        load_ratio = s.load->value.one / ncpu;
        add_ev(d, "load_1m",
               "load average " + fmt::num(s.load->value.one, 2) + " / " + fmt::num(s.load->value.five, 2) + " / " +
                   fmt::num(s.load->value.fifteen, 2) + " (" + fmt::num(*load_ratio, 2) + " per CPU)",
               s.load->prov.source, s.load->value.one);
    }
    if (s.runnable_tasks) {
        add_ev(d, "runnable_tasks", std::to_string(*s.runnable_tasks) + " runnable tasks right now", "procfs:/proc/stat",
               static_cast<double>(*s.runnable_tasks));
    }

    // Per-process attribution.
    double visible_cores = 0.0;
    if (s.processes) {
        for (const auto& p : s.processes->value)
            if (p.cpu_pct) visible_cores += *p.cpu_pct / 100.0;
    } else {
        note_unavailable(d, s, ObservationKind::process_list, "per-process CPU");
    }
    const auto top = s.top_by_cpu(5);
    for (const auto* p : top) {
        d.ranked_pids.push_back(p->info.pid);
        add_ev(d, "proc_cpu:" + std::to_string(p->info.pid),
               proc_label(p->info) + " " + fmt::pct(*p->cpu_pct) + " of one CPU" +
                   (p->info.threads ? ", " + std::to_string(*p->info.threads) + " threads" : std::string{}),
               s.processes->prov.source, *p->cpu_pct, Validity::derived);
    }

    const double top_share = (!top.empty() && busy_cores > 0.05) ? (*top[0]->cpu_pct / 100.0) / busy_cores : 0.0;
    const bool dominant = top_share >= t_.dominant_share && !top.empty() && *top[0]->cpu_pct >= 20.0;

    if (busy >= t_.cpu_busy_high_pct) {
        d.conclusion = dominant ? "cpu_saturated_dominated_by_process" : "cpu_saturated";
        d.headline = "CPU is saturated (" + fmt::pct(busy) + " busy)" +
                     (dominant ? std::string(", mostly by ") + proc_label(top[0]->info) + "." : ".");
    } else if (busy <= t_.cpu_busy_low_pct) {
        d.conclusion = "cpu_mostly_idle";
        d.headline = "CPU is mostly idle right now (" + fmt::pct(busy) + " busy).";
    } else if (dominant) {
        d.conclusion = "cpu_dominated_by_process";
        d.headline = proc_label(top[0]->info) + " accounts for most CPU use (" + fmt::pct(*top[0]->cpu_pct) +
                     " of one CPU; machine " + fmt::pct(busy) + " busy).";
    } else {
        d.conclusion = "cpu_moderately_busy";
        d.headline = "CPU is moderately busy (" + fmt::pct(busy) + "), spread across several processes.";
    }
    if (dominant) d.subject_pid = top[0]->info.pid;

    if (!top.empty()) {
        std::string t = "Top CPU consumers: ";
        for (std::size_t i = 0; i < top.size() && i < 3; ++i) {
            if (i) t += ", ";
            t += proc_label(top[i]->info) + " " + fmt::pct(*top[i]->cpu_pct);
        }
        std::vector<std::string> ev;
        ev.reserve(top.size());
        for (const auto* p : top) ev.push_back("proc_cpu:" + std::to_string(p->info.pid));
        d.findings.push_back(finding(FindingKind::observation, "top_cpu_processes", t, ev, Certainty::high));
        const double tp = *top[0]->cpu_pct;
        if (tp >= t_.single_thread_lo_pct && tp <= t_.single_thread_hi_pct && ncpu > 1) {
            d.findings.push_back(finding(
                FindingKind::hypothesis, "single_thread_bound",
                proc_label(top[0]->info) + " uses about one full CPU. That pattern is consistent with one busy thread "
                "(e.g. a spin loop or a single-threaded computation); per-thread data would be needed to confirm.",
                {"proc_cpu:" + std::to_string(top[0]->info.pid)}, Certainty::limited));
        } else if (tp > t_.single_thread_hi_pct) {
            d.findings.push_back(finding(FindingKind::observation, "multi_core_process",
                                         proc_label(top[0]->info) + " is using about " + fmt::num(tp / 100.0, 1) +
                                             " CPUs in parallel (multi-threaded).",
                                         {"proc_cpu:" + std::to_string(top[0]->info.pid)}, Certainty::high));
        }
    }

    // Saturation: demand vs. capacity.
    // Linux reports the instantaneous runnable count; prefer it over the
    // exponentially-damped load average, which lags by about a minute.
    const bool queueing = s.runnable_tasks ? *s.runnable_tasks > ncpu
                                           : (load_ratio && *load_ratio > t_.load_per_cpu_high);
    if (!queueing && s.runnable_tasks && load_ratio && *load_ratio > t_.load_per_cpu_high) {
        d.findings.push_back(finding(FindingKind::correlation, "load_average_lags",
                                     "The 1-minute load average exceeds the CPU count, but few tasks are runnable "
                                     "right now: the demand was recent and has subsided (Linux load also counts "
                                     "tasks blocked in uninterruptible I/O).",
                                     {"load_1m", "runnable_tasks"}, Certainty::moderate));
    }
    if (queueing) {
        Finding f = finding(FindingKind::causal_explanation, "run_queue_contention",
                            "More runnable work than logical CPUs: some threads must wait in run queues, which adds "
                            "scheduling latency.",
                            {s.runnable_tasks ? "runnable_tasks" : "load_1m"}, Certainty::moderate);
        if (auto m = mechanism(ck::cpu_saturation, ck::run_queue)) f.mechanisms.push_back(*m);
        if (auto m = mechanism(ck::run_queue, ck::scheduling_latency)) f.mechanisms.push_back(*m);
        if (auto m = mechanism(ck::scheduling_latency, ck::responsiveness)) f.mechanisms.push_back(*m);
        d.findings.push_back(std::move(f));
    } else if (busy >= t_.cpu_busy_high_pct && load_ratio) {
        d.findings.push_back(finding(FindingKind::correlation, "busy_without_queue",
                                     "CPUs are busy but the load average does not exceed the CPU count, so little "
                                     "queueing is indicated.",
                                     {"cpu_busy_pct", "load_1m"}, Certainty::limited));
    }

    // Attribution gap: CPU time used by processes OSIE cannot read.
    if (s.processes && busy_cores > 0.5) {
        const double gap = std::max(0.0, busy_cores - visible_cores);
        if (gap / busy_cores >= t_.unattributed_share && s.processes_partial > 0) {
            add_ev(d, "cpu_unattributed_cores", fmt::num(gap, 2) + " CPUs of busy time not attributable to readable processes",
                   "derived", gap, Validity::derived);
            d.findings.push_back(finding(
                FindingKind::hypothesis, "cpu_in_unreadable_processes",
                fmt::num(100.0 * gap / busy_cores, 0) + "% of busy CPU time is not attributable to processes OSIE can "
                "read; it likely belongs to the " + std::to_string(s.processes_partial) +
                " system/other-user processes (or the kernel) that are only partially visible without privileges.",
                {"cpu_busy_pct", "cpu_unattributed_cores"}, Certainty::limited));
        }
    }
    if (s.processes_partial > 0) {
        d.missing_evidence.push_back("CPU time of " + std::to_string(s.processes_partial) +
                                     " processes owned by other users (requires privileges OSIE does not request)");
    }

    // Platform specifics.
    if (platform_ == Platform::macos) {
        if (s.topology && s.topology->value.clusters.size() > 1) {
            std::string c;
            for (const auto& cl : s.topology->value.clusters) {
                if (!c.empty()) c += " + ";
                c += std::to_string(cl.logical_cpus) + " " + cl.name;
            }
            add_ev(d, "cpu_clusters", "heterogeneous CPU: " + c, s.topology->prov.source, std::nullopt);
            Finding f = finding(FindingKind::observation, "heterogeneous_cores",
                                "This Mac mixes performance and efficiency cores (" + c +
                                    "); utilisation % counts both equally although efficiency cores do less work per "
                                    "second, and background-QoS work is preferentially placed on them.",
                                {"cpu_clusters"}, Certainty::moderate);
            if (auto m = mechanism(ck::qos_classes, ck::heterogeneous_cores)) f.mechanisms.push_back(*m);
            d.findings.push_back(std::move(f));
        }
        if (s.power) {
            const auto th = s.power->value.thermal;
            add_ev(d, "thermal_state", "thermal state " + std::string(to_string(th)) +
                                           (s.power->value.low_power_mode ? ", Low Power Mode on" : ""),
                   s.power->prov.source, static_cast<double>(th));
            if (th >= ThermalState::serious) {
                Finding f = finding(FindingKind::hypothesis, "thermal_throttling_possible",
                                    "Thermal state is " + std::string(to_string(th)) +
                                        "; macOS may be reducing CPU performance, so the same work keeps CPUs busy longer.",
                                    {"thermal_state"}, Certainty::limited);
                if (auto m = mechanism(ck::thermal_pressure, ck::cpu_frequency)) f.mechanisms.push_back(*m);
                if (auto m = mechanism(ck::cpu_frequency, ck::cpu_saturation)) f.mechanisms.push_back(*m);
                d.findings.push_back(std::move(f));
            }
            if (s.power->value.low_power_mode) {
                d.findings.push_back(finding(FindingKind::observation, "low_power_mode",
                                             "Low Power Mode is enabled, which reduces performance to save energy.",
                                             {"thermal_state"}, Certainty::high));
            }
        } else {
            note_unavailable(d, s, ObservationKind::thermal_state, "thermal state");
        }
        d.missing_evidence.push_back("CPU temperature and clock frequency (no public API on Apple silicon)");
    } else {
        if (u.iowait_pct) {
            add_ev(d, "cpu_iowait_pct", "iowait " + fmt::pct(*u.iowait_pct), src, *u.iowait_pct, Validity::derived);
            if (*u.iowait_pct >= t_.iowait_high_pct) {
                d.findings.push_back(finding(FindingKind::correlation, "high_iowait",
                                             "High iowait: CPUs are idle while tasks wait for I/O; the bottleneck may "
                                             "be storage rather than CPU.",
                                             {"cpu_iowait_pct"}, Certainty::limited));
            }
        }
        if (u.steal_pct) {
            add_ev(d, "cpu_steal_pct", "steal " + fmt::pct(*u.steal_pct), src, *u.steal_pct, Validity::derived);
            if (*u.steal_pct >= t_.steal_high_pct) {
                Finding f = finding(FindingKind::hypothesis, "hypervisor_contention",
                                    "Significant steal time: the hypervisor is running other guests on this VM's CPUs.",
                                    {"cpu_steal_pct"}, Certainty::moderate);
                if (auto m = mechanism(ck::hypervisor_steal, ck::scheduling_latency)) f.mechanisms.push_back(*m);
                d.findings.push_back(std::move(f));
            }
        }
        if (const auto* p = psi_of(s, PsiResource::cpu)) {
            add_ev(d, "psi_cpu_some_avg10", "PSI cpu some avg10 " + fmt::pct(p->some.avg10, 2), s.psi->prov.source,
                   p->some.avg10);
            if (p->some.avg10 >= t_.psi_some_high) {
                Finding f = finding(FindingKind::causal_explanation, "cpu_stall_measured",
                                    "Tasks spent " + fmt::pct(p->some.avg10) +
                                        " of the last 10 s waiting for a CPU (PSI) - direct evidence of CPU contention.",
                                    {"psi_cpu_some_avg10"}, Certainty::high);
                if (auto m = mechanism(ck::cpu_saturation, ck::psi_cpu)) f.mechanisms.push_back(*m);
                d.findings.push_back(std::move(f));
            }
        } else {
            note_unavailable(d, s, ObservationKind::pressure_stall, "PSI");
        }
        for (const auto& cg : s.cgroups) {
            if (!cg.info.cpu_quota_us) continue;
            const std::string id = "cgroup_throttled:" + cg.info.path;
            const double quota_cpus = static_cast<double>(*cg.info.cpu_quota_us) /
                                      static_cast<double>(std::max<std::uint64_t>(1, cg.info.cpu_period_us.value_or(100000)));
            // Prefer the rate measured over the sampling window; the lifetime
            // ratio averages over the cgroup's whole life and can hide (or
            // invent) current throttling.
            const bool windowed = cg.window_throttled_ratio.has_value();
            const auto ratio = windowed ? cg.window_throttled_ratio : cg.lifetime_throttled_ratio;
            std::string text = "cgroup " + cg.info.path + " quota " + fmt::num(quota_cpus, 2) + " CPUs (cpu.max " +
                               std::to_string(*cg.info.cpu_quota_us) + "/" +
                               std::to_string(cg.info.cpu_period_us.value_or(100000)) + " us), throttled in " +
                               fmt::pct(100.0 * ratio.value_or(0.0)) + " of periods " +
                               (windowed ? "over the last " + fmt::num(*cg.window_s, 1) + " s" : std::string("(lifetime average)"));
            if (windowed && cg.window_usage_cpus) text += ", using " + fmt::num(*cg.window_usage_cpus, 2) + " CPUs";
            if (windowed && cg.window_throttled_cpus)
                text += ", " + fmt::num(*cg.window_throttled_cpus, 2) + " CPU-seconds/s of throttled time";
            if (!cg.window_note.empty()) text += " (" + cg.window_note + ")";
            add_ev(d, id, text, "cgroupfs:cpu.stat", ratio, windowed ? Validity::derived : Validity::valid);
            if (ratio && *ratio >= t_.throttled_ratio_high) {
                Finding f = finding(FindingKind::causal_explanation, "cgroup_cpu_throttling",
                                    "cgroup " + cg.info.path + " hits its CPU quota" +
                                        (windowed ? std::string(" right now") : std::string(" (lifetime average; current rate not measured)")) +
                                        ": its tasks are throttled until the next period even if other CPUs are idle.",
                                    {id}, windowed ? Certainty::high : Certainty::limited);
                if (auto m = mechanism(ck::cpu_quota, ck::cpu_throttling)) f.mechanisms.push_back(*m);
                if (auto m = mechanism(ck::cpu_throttling, ck::scheduling_latency)) f.mechanisms.push_back(*m);
                d.findings.push_back(std::move(f));
            }
            // A cgroup at its quota is CPU-bound even when the host is idle:
            // utilisation of all logical CPUs is the wrong denominator for it.
            if (windowed && cg.window_usage_cpus && *cg.window_usage_cpus >= 0.9 * quota_cpus && ratio &&
                *ratio >= t_.throttled_ratio_high && busy < t_.cpu_busy_high_pct) {
                d.conclusion = "cpu_quota_bound";
                d.headline = "CPU is limited by a cgroup quota: " + cg.info.path + " used " +
                             fmt::num(*cg.window_usage_cpus, 2) + " of its " + fmt::num(quota_cpus, 2) +
                             "-CPU quota and was throttled in " + fmt::pct(100.0 * *ratio) +
                             " of periods, while the machine as a whole is " + fmt::pct(busy) + " busy.";
            }
        }
    }

    if (dominant && top[0]->info.pid != ::getpid()) {
        d.findings.push_back(finding(
            FindingKind::proposed_optimization, "reduce_dominant_process",
            "If " + top[0]->info.name + " is not expected to be busy, quitting or pausing it would free about " +
                fmt::num(*top[0]->cpu_pct / 100.0, 1) + " CPUs. (Not executed: OSIE V1 is read-only.)",
            {"proc_cpu:" + std::to_string(top[0]->info.pid)}, Certainty::limited));
    }
    d.missing_evidence.push_back("CPU history: only a " + fmt::num(window_s, 2) + " s window was sampled");
    d.missing_evidence.push_back("per-thread CPU breakdown");

    d.next_step = dominant ? "ask why " + top[0]->info.name + " is using CPU, or re-check in a few seconds"
                           : (d.conclusion == "cpu_mostly_idle" ? "if slowness persists, check memory pressure"
                                                                : "re-sample to see whether the load is sustained");
    const bool partial = s.processes && s.processes->prov.validity == Validity::partial;
    d.certainty = partial ? Certainty::limited : Certainty::moderate;
    return d;
}

// ------------------------------------------------------------- memory ----

Diagnosis Reasoner::memory(const SystemState& s) const {
    Diagnosis d;
    d.topic = "memory";
    if (!s.memory) {
        d.conclusion = "insufficient_evidence";
        d.headline = "Memory statistics could not be observed.";
        d.certainty = Certainty::none;
        note_unavailable(d, s, ObservationKind::memory, "memory statistics");
        return d;
    }
    const auto& m = s.memory->value;
    const auto src = s.memory->prov.source;
    const double total = static_cast<double>(std::max<std::uint64_t>(1, m.total_bytes));

    int severity = 0;  // 0 ok, 1 elevated, 2 high
    std::vector<std::string> pressure_ev;
    bool major_fault_note = false;

    add_ev(d, "mem_total", "physical memory " + fmt::bytes(m.total_bytes), src, total);
    if (m.available_bytes) {
        const double share = static_cast<double>(*m.available_bytes) / total;
        add_ev(d, "mem_available_share",
               "available (estimate) " + fmt::bytes(*m.available_bytes) + " = " + fmt::pct(100 * share), src, share);
        if (share < t_.available_share_low) {
            severity = std::max(severity, 1);
            pressure_ev.emplace_back("mem_available_share");
        }
    }
    if (m.pressure_level) {
        add_ev(d, "mem_pressure_level", "kernel memory-pressure level: " + std::string(to_string(*m.pressure_level)),
               "sysctl:kern.memorystatus_vm_pressure_level", static_cast<double>(*m.pressure_level));
        if (*m.pressure_level == MemoryPressureLevel::critical) severity = 2;
        if (*m.pressure_level == MemoryPressureLevel::warning) severity = std::max(severity, 1);
        if (*m.pressure_level != MemoryPressureLevel::normal) pressure_ev.emplace_back("mem_pressure_level");
    }
    if (m.compressed_bytes) {
        const double share = static_cast<double>(*m.compressed_bytes) / total;
        add_ev(d, "mem_compressed_share", "compressor holds " + fmt::bytes(*m.compressed_bytes) + " (" +
                                              fmt::pct(100 * share) + " of RAM)", src, share);
        if (share >= t_.compressed_share_high) {
            severity = std::max(severity, 1);
            pressure_ev.emplace_back("mem_compressed_share");
        }
    }
    if (m.swap_used_bytes) {
        add_ev(d, "swap_used", "swap used " + fmt::bytes(*m.swap_used_bytes) + " of " +
                                   fmt::bytes(m.swap_total_bytes.value_or(0)), src,
               static_cast<double>(*m.swap_used_bytes));
    }
    if (s.memory_rates) {
        const auto& r = s.memory_rates->value;
        const auto rs = s.memory_rates->prov.source;
        if (r.swapouts_per_s) {
            add_ev(d, "swapouts_per_s", "swap-outs " + fmt::num(*r.swapouts_per_s) + "/s", rs, *r.swapouts_per_s,
                   Validity::derived);
            if (*r.swapouts_per_s >= t_.swapouts_per_s_high) {
                severity = std::max(severity, 1);
                pressure_ev.emplace_back("swapouts_per_s");
            }
        }
        if (r.pageouts_per_s) {
            add_ev(d, "pageouts_per_s", "page-outs " + fmt::num(*r.pageouts_per_s) + "/s", rs, *r.pageouts_per_s,
                   Validity::derived);
            if (*r.pageouts_per_s >= t_.pageouts_per_s_high) {
                severity = std::max(severity, 1);
                pressure_ev.emplace_back("pageouts_per_s");
            }
        }
        if (r.compressions_per_s) {
            add_ev(d, "compressions_per_s", "compressions " + fmt::num(*r.compressions_per_s) + "/s", rs,
                   *r.compressions_per_s, Validity::derived);
        }
        if (r.swapins_per_s) {
            add_ev(d, "swapins_per_s", "swap-ins " + fmt::num(*r.swapins_per_s) + "/s", rs, *r.swapins_per_s,
                   Validity::derived);
        }
        if (r.major_faults_per_s) {
            add_ev(d, "major_faults_per_s", "major faults " + fmt::num(*r.major_faults_per_s) + "/s", rs,
                   *r.major_faults_per_s, Validity::derived);
            // Major faults also count cold reads of executables and mapped files,
            // so they indicate pressure only when swap-ins corroborate them.
            if (*r.major_faults_per_s >= t_.major_faults_per_s_high) {
                if (r.swapins_per_s.value_or(0.0) >= t_.swapouts_per_s_high) {
                    severity = std::max(severity, 1);
                    pressure_ev.emplace_back("major_faults_per_s");
                    pressure_ev.emplace_back("swapins_per_s");
                } else {
                    major_fault_note = true;
                }
            }
        }
    }
    if (const auto* p = psi_of(s, PsiResource::memory)) {
        add_ev(d, "psi_memory_some_avg10", "PSI memory some avg10 " + fmt::pct(p->some.avg10, 2), s.psi->prov.source,
               p->some.avg10);
        if (p->some.avg10 >= t_.psi_some_high) {
            severity = std::max(severity, 1);
            pressure_ev.emplace_back("psi_memory_some_avg10");
        }
        if (p->full && p->full->avg10 >= t_.psi_full_high) {
            add_ev(d, "psi_memory_full_avg10", "PSI memory full avg10 " + fmt::pct(p->full->avg10, 2),
                   s.psi->prov.source, p->full->avg10);
            severity = 2;
            pressure_ev.emplace_back("psi_memory_full_avg10");
        }
    } else if (platform_ == Platform::linux_os) {
        note_unavailable(d, s, ObservationKind::pressure_stall, "PSI memory");
    }

    const auto top = s.top_by_memory(5);
    for (const auto* p : top) {
        d.ranked_pids.push_back(p->info.pid);
        add_ev(d, "proc_mem:" + std::to_string(p->info.pid),
               proc_label(p->info) + " " + fmt::bytes(*p->info.memory_bytes()) + " (" +
                   fmt::pct(100.0 * static_cast<double>(*p->info.memory_bytes()) / total) + " of RAM)",
               s.processes->prov.source, static_cast<double>(*p->info.memory_bytes()));
    }

    if (severity == 2) {
        d.conclusion = "memory_pressure_high";
        d.headline = "Memory pressure is high: the system is actively reclaiming or swapping memory.";
    } else if (severity == 1) {
        d.conclusion = "possible_memory_pressure";
        d.headline = "There are signs of memory pressure.";
    } else {
        d.conclusion = "memory_ok";
        d.headline = "No memory pressure is indicated right now.";
    }

    if (severity > 0) {
        Finding f = finding(severity == 2 ? FindingKind::causal_explanation : FindingKind::hypothesis,
                            "memory_pressure", "Memory demand is close to what RAM can hold, so the kernel is "
                            "reclaiming pages" + std::string(platform_ == Platform::macos ? ", compressing memory" : "") +
                            " and may page to disk; touching paged-out memory stalls threads.",
                            pressure_ev, severity == 2 ? Certainty::moderate : Certainty::limited);
        if (auto mm = mechanism(ck::memory_pressure, ck::paging)) f.mechanisms.push_back(*mm);
        if (platform_ == Platform::macos)
            if (auto mm = mechanism(ck::memory_pressure, ck::memory_compression)) f.mechanisms.push_back(*mm);
        if (auto mm = mechanism(ck::swapping, ck::major_faults)) f.mechanisms.push_back(*mm);
        if (auto mm = mechanism(ck::major_faults, ck::responsiveness)) f.mechanisms.push_back(*mm);
        d.findings.push_back(std::move(f));
    }
    if (m.swap_used_bytes && *m.swap_used_bytes > 0 &&
        (!s.memory_rates || s.memory_rates->value.swapouts_per_s.value_or(0) < t_.swapouts_per_s_high)) {
        d.findings.push_back(finding(FindingKind::correlation, "historical_swap",
                                     "Swap is in use but little or no swapping happened during the sample: pages were "
                                     "likely swapped out earlier and are not necessarily causing a slowdown now.",
                                     {"swap_used"}, Certainty::limited));
    }
    if (major_fault_note) {
        d.findings.push_back(finding(FindingKind::correlation, "major_faults_without_swapin",
                                     "Many major page faults but little swap-in activity: these are most likely first "
                                     "reads of program files or mapped data, not memory pressure.",
                                     {"major_faults_per_s"}, Certainty::limited));
    }
    if (severity == 0 && m.free_bytes && static_cast<double>(*m.free_bytes) / total < 0.1) {
        d.findings.push_back(finding(FindingKind::observation, "low_free_is_normal",
                                     "Little memory is completely free, which is normal: the OS keeps file data cached "
                                     "in otherwise unused RAM and releases it on demand.",
                                     {"mem_available_share"}, Certainty::high));
    }
    if (!top.empty()) {
        std::string t = "Largest memory users: ";
        for (std::size_t i = 0; i < top.size() && i < 3; ++i) {
            if (i) t += ", ";
            t += proc_label(top[i]->info) + " " + fmt::bytes(*top[i]->info.memory_bytes());
        }
        std::vector<std::string> ev;
        for (const auto* p : top) ev.push_back("proc_mem:" + std::to_string(p->info.pid));
        d.findings.push_back(finding(FindingKind::observation, "top_memory_processes", t, ev, Certainty::high));
        if (severity > 0 && top[0]->info.pid != ::getpid()) {
            d.findings.push_back(finding(FindingKind::proposed_optimization, "free_memory",
                                         "Quitting large applications you are not using (e.g. " + top[0]->info.name +
                                             ") would relieve pressure. (Not executed: OSIE V1 is read-only.)",
                                         {"proc_mem:" + std::to_string(top[0]->info.pid)}, Certainty::limited));
        }
    }
    if (s.processes_partial > 0) {
        d.missing_evidence.push_back("memory of " + std::to_string(s.processes_partial) +
                                     " processes owned by other users (requires privileges)");
    }
    d.missing_evidence.push_back("memory usage trend over time (single sample)");
    if (platform_ == Platform::macos) d.missing_evidence.push_back("per-process compressed/swapped bytes");
    d.next_step = severity > 0 ? "inspect_memory_trends: re-check in a minute and look at the largest processes"
                               : "if the system feels slow, check CPU and disk instead";
    d.certainty = severity == 2 ? Certainty::moderate : Certainty::limited;
    return d;
}

// ----------------------------------------------------------- slowness ----

Diagnosis Reasoner::slowness(const SystemState& s) const {
    Diagnosis c = cpu(s);
    Diagnosis m = memory(s);
    Diagnosis d;
    d.topic = "slowness";

    auto merge = [&d](const Diagnosis& x) {
        for (const auto& e : x.evidence)
            if (!d.find_evidence(e.id)) d.evidence.push_back(e);
        for (const auto& f : x.findings) d.findings.push_back(f);
        for (const auto& me : x.missing_evidence)
            if (std::find(d.missing_evidence.begin(), d.missing_evidence.end(), me) == d.missing_evidence.end())
                d.missing_evidence.push_back(me);
    };
    merge(c);
    merge(m);

    std::vector<std::string> causes;
    int score = 0;
    if (m.conclusion == "memory_pressure_high") {
        causes.emplace_back("high memory pressure");
        score = 3;
    }
    if (c.conclusion.rfind("cpu_saturated", 0) == 0) {
        causes.emplace_back("CPU saturation");
        score = std::max(score, 3);
    }
    if (m.conclusion == "possible_memory_pressure") {
        causes.emplace_back("possible memory pressure");
        score = std::max(score, 2);
    }
    if (c.conclusion == "cpu_dominated_by_process") {
        causes.emplace_back("one process using much CPU");
        score = std::max(score, 1);
    }
    for (const auto& f : d.findings) {
        if (f.code == "thermal_throttling_possible") causes.emplace_back("thermal throttling");
        if (f.code == "cgroup_cpu_throttling") causes.emplace_back("cgroup CPU throttling");
        if (f.code == "high_iowait") causes.emplace_back("I/O wait");
    }
    if (const auto* p = psi_of(s, PsiResource::io)) {
        add_ev(d, "psi_io_some_avg10", "PSI io some avg10 " + fmt::pct(p->some.avg10, 2), s.psi->prov.source,
               p->some.avg10);
        if (p->some.avg10 >= t_.psi_some_high) {
            causes.emplace_back("I/O stalls");
            Finding f = finding(FindingKind::causal_explanation, "io_stall_measured",
                                "Tasks spent " + fmt::pct(p->some.avg10) + " of the last 10 s stalled on I/O (PSI).",
                                {"psi_io_some_avg10"}, Certainty::high);
            if (auto mm = mechanism(ck::io_latency, ck::responsiveness)) f.mechanisms.push_back(*mm);
            d.findings.push_back(std::move(f));
            score = std::max(score, 2);
        }
    }
    if (s.filesystems) {
        for (const auto& fs : s.filesystems->value) {
            if (fs.total_bytes < (1ULL << 30) || fs.read_only || !fs.local) continue;
            const double used = 1.0 - static_cast<double>(fs.available_bytes) / static_cast<double>(fs.total_bytes);
            if (used >= t_.fs_full_share) {
                const std::string id = "fs_full:" + fs.mount_point;
                add_ev(d, id, fs.mount_point + " is " + fmt::pct(100 * used) + " full", s.filesystems->prov.source, used);
                d.findings.push_back(finding(FindingKind::hypothesis, "filesystem_nearly_full",
                                             fs.mount_point + " is nearly full; low free space can slow writes, "
                                             "swap-file growth and updates.",
                                             {id}, Certainty::low));
                causes.emplace_back("nearly full disk");
                score = std::max(score, 1);
            }
        }
    }
    d.ranked_pids = c.ranked_pids;
    if (causes.empty()) {
        d.conclusion = "no_current_bottleneck_observed";
        d.headline = "No CPU, memory" + std::string(platform_ == Platform::linux_os ? ", I/O" : "") +
                     " or disk-space bottleneck is visible right now.";
        d.missing_evidence.emplace_back("the slowdown may be intermittent: only a short window was sampled");
        d.missing_evidence.emplace_back("application-level latency, GPU and network latency are not observed");
        d.next_step = "ask again while the slowdown is happening";
        d.certainty = Certainty::limited;
    } else {
        std::string list;
        for (std::size_t i = 0; i < causes.size(); ++i) list += (i ? ", " : "") + causes[i];
        d.conclusion = score >= 3 ? "likely_resource_bottleneck" : "possible_resource_bottleneck";
        d.headline = "Most likely contributors right now: " + list + ".";
        d.next_step = c.subject_pid ? "ask about the top process" : m.next_step;
        d.certainty = score >= 3 ? Certainty::moderate : Certainty::limited;
    }
    return d;
}

// ------------------------------------------------------ per process ------

Diagnosis Reasoner::process_cpu(const SystemState& s, Pid pid) const {
    Diagnosis d;
    d.topic = "process_cpu";
    d.subject_pid = pid;
    const ProcessView* p = s.find_pid(pid);
    if (p == nullptr) {
        d.conclusion = "process_not_found";
        d.headline = "Process " + std::to_string(pid) + " is not running (it may have exited).";
        d.certainty = Certainty::high;
        return d;
    }
    const auto src = s.processes->prov.source;
    const auto& pi = p->info;
    if (!p->cpu_pct) {
        d.conclusion = pi.validity == Validity::partial ? "process_cpu_not_readable" : "process_cpu_no_baseline";
        d.headline = pi.validity == Validity::partial
                         ? proc_label(pi) + " belongs to another user; its CPU time is not readable without privileges."
                         : proc_label(pi) + " started during the sample, so no CPU rate is available yet.";
        d.certainty = Certainty::none;
        d.missing_evidence.push_back("process CPU time");
        return d;
    }
    const double cp = *p->cpu_pct;
    const double ncpu = std::max<std::uint32_t>(1, s.logical_cpus());
    add_ev(d, "proc_cpu", proc_label(pi) + " " + fmt::pct(cp) + " of one CPU (" + fmt::pct(cp / ncpu) +
                              " of the machine)", src, cp, Validity::derived);
    if (pi.threads) add_ev(d, "proc_threads", std::to_string(*pi.threads) + " threads", src, *pi.threads);
    add_ev(d, "proc_state", "state " + std::string(to_string(pi.state)), src, std::nullopt);
    if (pi.nice && *pi.nice != 0) add_ev(d, "proc_nice", "nice " + std::to_string(*pi.nice), src, *pi.nice);
    double child_cpu = 0;
    std::size_t nchild = 0;
    for (const auto* c : s.children_of(pid)) {
        ++nchild;
        if (c->cpu_pct) child_cpu += *c->cpu_pct;
    }
    if (nchild > 0) {
        add_ev(d, "children_cpu", std::to_string(nchild) + " child processes using " + fmt::pct(child_cpu) + " CPU",
               src, child_cpu, Validity::derived);
    }

    if (cp < 5.0) {
        d.conclusion = "process_cpu_low";
        d.headline = proc_label(pi) + " is barely using CPU right now (" + fmt::pct(cp) + ").";
    } else if (cp >= t_.single_thread_lo_pct && cp <= t_.single_thread_hi_pct) {
        d.conclusion = "process_single_core_saturated";
        d.headline = proc_label(pi) + " is using about one full CPU core (" + fmt::pct(cp) + ").";
        d.findings.push_back(finding(FindingKind::hypothesis, "single_thread_bound",
                                     "Usage pinned near 100% of one core typically means one thread is running "
                                     "continuously (a long computation, a busy-wait, or a loop). The process has " +
                                         std::to_string(pi.threads.value_or(0)) +
                                         " threads, but OSIE V1 does not read per-thread CPU to confirm which one.",
                                     {"proc_cpu", "proc_threads"}, Certainty::limited));
    } else if (cp > t_.single_thread_hi_pct) {
        d.conclusion = "process_multi_core";
        d.headline = proc_label(pi) + " is using about " + fmt::num(cp / 100.0, 1) + " CPU cores in parallel.";
        d.findings.push_back(finding(FindingKind::observation, "parallel_work",
                                     "More than one core in use means several threads are runnable at once "
                                     "(parallel computation, compilation, rendering, or many busy workers).",
                                     {"proc_cpu", "proc_threads"}, Certainty::high));
    } else {
        d.conclusion = "process_cpu_moderate";
        d.headline = proc_label(pi) + " is using " + fmt::pct(cp) + " of one CPU.";
    }
    if (cp / 100.0 >= 0.5 * ncpu) {
        Finding f = finding(FindingKind::hypothesis, "competes_for_cpu",
                            "It occupies at least half of all CPUs, so other runnable work is more likely to wait.",
                            {"proc_cpu"}, Certainty::limited);
        if (auto m = mechanism(ck::run_queue, ck::scheduling_latency)) f.mechanisms.push_back(*m);
        d.findings.push_back(std::move(f));
    }
    if (platform_ == Platform::linux_os && pi.cgroup) {
        for (const auto& cg : s.cgroups) {
            if (cg.info.path != *pi.cgroup || !cg.lifetime_throttled_ratio) continue;
            add_ev(d, "proc_cgroup_throttled", "its cgroup " + cg.info.path + " was throttled in " +
                                                   fmt::pct(100 * *cg.lifetime_throttled_ratio) + " of periods",
                   "cgroupfs:cpu.stat", *cg.lifetime_throttled_ratio);
        }
    }
    d.missing_evidence.emplace_back("per-thread CPU and call stacks (not read by OSIE V1)");
    d.missing_evidence.emplace_back("how long it has been this busy (single sample)");
    d.next_step = "re-check in a few seconds to see whether the usage is sustained";
    d.certainty = Certainty::limited;
    return d;
}

Diagnosis Reasoner::process_memory(const SystemState& s, Pid pid) const {
    Diagnosis d;
    d.topic = "process_memory";
    d.subject_pid = pid;
    const ProcessView* p = s.find_pid(pid);
    if (p == nullptr) {
        d.conclusion = "process_not_found";
        d.headline = "Process " + std::to_string(pid) + " is not running (it may have exited).";
        d.certainty = Certainty::high;
        return d;
    }
    const auto& pi = p->info;
    const auto src = s.processes->prov.source;
    const auto mem = pi.memory_bytes();
    if (!mem) {
        d.conclusion = "process_memory_not_readable";
        d.headline = proc_label(pi) + " belongs to another user; its memory is not readable without privileges.";
        d.certainty = Certainty::none;
        d.missing_evidence.emplace_back("process memory counters");
        return d;
    }
    const double total = s.memory ? static_cast<double>(s.memory->value.total_bytes)
                                  : (s.system ? static_cast<double>(s.system->value.mem_total_bytes) : 0.0);
    const double share = total > 0 ? static_cast<double>(*mem) / total : 0.0;
    add_ev(d, "proc_memory", proc_label(pi) + " uses " + fmt::bytes(*mem) +
                                 (total > 0 ? " (" + fmt::pct(100 * share) + " of RAM)" : std::string{}),
           src, static_cast<double>(*mem));
    if (pi.footprint_bytes && pi.resident_bytes) {
        add_ev(d, "proc_footprint_vs_resident",
               "footprint " + fmt::bytes(*pi.footprint_bytes) + " vs resident " + fmt::bytes(*pi.resident_bytes), src,
               static_cast<double>(*pi.footprint_bytes));
    }
    if (pi.virtual_bytes) add_ev(d, "proc_virtual", "virtual size " + fmt::bytes(*pi.virtual_bytes), src,
                                 static_cast<double>(*pi.virtual_bytes));
    if (pi.threads) add_ev(d, "proc_threads", std::to_string(*pi.threads) + " threads", src, *pi.threads);

    // Multi-process applications: children + same-named helpers.
    std::uint64_t family = 0;
    std::size_t family_n = 0;
    const std::string stem = pi.name.substr(0, pi.name.find(' '));
    if (s.processes) {
        for (const auto& q : s.processes->value) {
            if (q.info.pid == pid || !q.info.memory_bytes()) continue;
            const bool child = q.info.ppid == pid;
            const bool helper = stem.size() >= 4 && q.info.name.rfind(stem, 0) == 0;
            if (child || helper) {
                family += *q.info.memory_bytes();
                ++family_n;
            }
        }
    }
    if (family_n > 0) {
        add_ev(d, "proc_family_memory", std::to_string(family_n) + " related processes (children or '" + stem +
                                            "*' helpers) use another " + fmt::bytes(family),
               src, static_cast<double>(family), Validity::derived);
    }

    d.conclusion = share >= 0.25 ? "process_memory_large" : (share >= 0.05 ? "process_memory_notable" : "process_memory_small");
    d.headline = proc_label(pi) + " is using " + fmt::bytes(*mem) +
                 (total > 0 ? " (" + fmt::pct(100 * share) + " of RAM)." : std::string("."));

    d.findings.push_back(finding(FindingKind::observation, "memory_metric_meaning",
                                 platform_ == Platform::macos
                                     ? "This is the process's physical footprint: memory it is responsible for, "
                                       "including compressed pages, excluding shared file cache. Its virtual size is "
                                       "much larger by design and is not a measure of RAM use."
                                     : "This is resident set size (RSS): pages currently in RAM, including shared "
                                       "pages counted in every process that maps them.",
                                 {"proc_memory"}, Certainty::high));
    d.findings.push_back(finding(FindingKind::hypothesis, "working_set_or_cache",
                                 "Large usage is most often by design: caches, loaded documents/media, JIT heaps or "
                                 "per-tab/per-view state that grows with what is open.",
                                 {"proc_memory"}, Certainty::low));
    if (family_n > 0) {
        d.findings.push_back(finding(FindingKind::observation, "multi_process_app",
                                     "The application is split across " + std::to_string(family_n + 1) +
                                         " processes; together they use " + fmt::bytes(*mem + family) + ".",
                                     {"proc_memory", "proc_family_memory"}, Certainty::moderate));
    }
    if (pi.threads && *pi.threads >= 100) {
        d.findings.push_back(finding(FindingKind::hypothesis, "many_threads",
                                     std::to_string(*pi.threads) + " threads each need a stack and bookkeeping; "
                                     "this contributes, but rarely dominates, memory use.",
                                     {"proc_threads"}, Certainty::low));
    }
    d.findings.push_back(finding(FindingKind::hypothesis, "possible_leak",
                                 "A memory leak is possible only if usage keeps growing without a corresponding "
                                 "workload; one sample cannot show that.",
                                 {"proc_memory"}, Certainty::low));
    const auto sysmem = memory(s);
    if (sysmem.conclusion != "memory_ok" && share >= 0.05) {
        Finding f = finding(FindingKind::correlation, "contributes_to_pressure",
                            "The system shows memory pressure and this process holds " + fmt::pct(100 * share) +
                                " of RAM, so it is a likely contributor.",
                            {"proc_memory"}, Certainty::limited);
        if (auto m = mechanism(ck::memory_pressure, ck::paging)) f.mechanisms.push_back(*m);
        d.findings.push_back(std::move(f));
        for (const auto& e : sysmem.evidence)
            if (e.id == "mem_pressure_level" || e.id == "mem_compressed_share") d.evidence.push_back(e);
    }
    d.missing_evidence.emplace_back("memory growth over time (needs repeated samples)");
    d.missing_evidence.emplace_back("heap/allocation breakdown (requires debugging privileges OSIE does not use)");
    d.next_step = "ask again later to compare; growth without more open work suggests a leak";
    d.certainty = Certainty::limited;
    return d;
}

Diagnosis Reasoner::explain_concept(std::string_view key) const {
    Diagnosis d;
    d.topic = "concept";
    const auto id = arch::concept_node(g_, key);
    if (id == arch::kNoNode) {
        d.conclusion = "unknown_concept";
        d.headline = "OSIE's architecture model has no concept named '" + std::string(key) + "'.";
        d.certainty = Certainty::none;
        return d;
    }
    const auto& n = g_.node(id);
    d.conclusion = std::string(key);
    d.headline = n.label + (n.platforms.contains(platform_) ? "" : " (not applicable on this platform)");
    for (auto eid : g_.out_edges(id)) {
        const auto& e = g_.edge(eid);
        if (!e.platforms.contains(platform_)) continue;
        Finding f;
        f.kind = e.nature == arch::EdgeNature::invariant ? FindingKind::causal_explanation : FindingKind::hypothesis;
        f.code = std::string(arch::to_string(e.kind));
        f.text = n.label + " " + std::string(arch::to_string(e.kind)) + " " + g_.node(e.to).label +
                 (e.mechanism.empty() ? "" : ": " + e.mechanism);
        f.certainty = e.nature == arch::EdgeNature::invariant ? Certainty::high : Certainty::limited;
        f.mechanisms.push_back(Mechanism{n.label, std::string(arch::to_string(e.kind)), g_.node(e.to).label, e.nature,
                                         e.confidence, e.mechanism, e.doc_ref});
        d.findings.push_back(std::move(f));
    }
    for (const auto& kind : {arch::EdgeKind::affects, arch::EdgeKind::part_of, arch::EdgeKind::limits}) {
        for (auto from : g_.in(id, kind)) {
            const auto& fn = g_.node(from);
            if (fn.kind != arch::NodeKind::concept_ || !fn.platforms.contains(platform_)) continue;
            for (auto eid : g_.out_edges(from)) {
                const auto& e = g_.edge(eid);
                if (e.to != id || e.kind != kind || !e.platforms.contains(platform_)) continue;
                Finding f;
                f.kind = e.nature == arch::EdgeNature::invariant ? FindingKind::causal_explanation : FindingKind::hypothesis;
                f.code = "incoming_" + std::string(arch::to_string(kind));
                f.text = fn.label + " " + std::string(arch::to_string(kind)) + " " + n.label +
                         (e.mechanism.empty() ? "" : ": " + e.mechanism);
                f.certainty = e.nature == arch::EdgeNature::invariant ? Certainty::high : Certainty::limited;
                f.mechanisms.push_back(Mechanism{fn.label, std::string(arch::to_string(kind)), n.label, e.nature,
                                                 e.confidence, e.mechanism, e.doc_ref});
                d.findings.push_back(std::move(f));
            }
        }
    }
    d.certainty = Certainty::high;
    d.missing_evidence.emplace_back("documentation reference: " + n.doc_ref);
    return d;
}

// ---------------------------------------------------------------- JSON ---

std::string to_json(const Diagnosis& d) {
    json::Writer w;
    w.begin_object();
    w.kv("topic", d.topic).kv("diagnosis", d.conclusion).kv("headline", d.headline);
    w.kv("certainty", to_string(d.certainty));
    if (d.subject_pid) w.kv("subject_pid", static_cast<std::int64_t>(*d.subject_pid));
    w.key("evidence").begin_array();
    for (const auto& e : d.evidence) {
        w.begin_object().kv("id", e.id).kv("text", e.text).kv("source", e.source).kv("validity", to_string(e.validity));
        w.key("value").value(e.value);
        w.end_object();
    }
    w.end_array();
    w.key("findings").begin_array();
    for (const auto& f : d.findings) {
        w.begin_object().kv("kind", to_string(f.kind)).kv("code", f.code).kv("text", f.text)
            .kv("certainty", to_string(f.certainty));
        w.key("evidence").begin_array();
        for (const auto& e : f.evidence_ids) w.value(e);
        w.end_array();
        w.key("mechanisms").begin_array();
        for (const auto& m : f.mechanisms) {
            w.begin_object().kv("from", m.from).kv("relation", m.relation).kv("to", m.to)
                .kv("nature", arch::to_string(m.nature)).kv("confidence", static_cast<double>(m.confidence))
                .kv("doc", m.doc_ref).end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.key("missing_evidence").begin_array();
    for (const auto& m : d.missing_evidence) w.value(m);
    w.end_array();
    w.kv("recommended_next_step", d.next_step);
    w.end_object();
    return w.take();
}

}  // namespace osie::reason
