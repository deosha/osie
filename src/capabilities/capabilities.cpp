// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cctype>

#include "osie/capabilities/capability.hpp"
#include "osie/util/json.hpp"

namespace osie {

std::string_view to_string(RiskLevel r) noexcept {
    switch (r) {
        case RiskLevel::none: return "none";
        case RiskLevel::low: return "low";
        case RiskLevel::medium: return "medium";
        case RiskLevel::high: return "high";
        case RiskLevel::critical: return "critical";
    }
    return "?";
}

std::string PermissionSet::to_string() const {
    static constexpr std::pair<Permission, std::string_view> names[] = {
        {Permission::read_own_processes, "read_own_processes"},
        {Permission::read_other_processes, "read_other_processes"},
        {Permission::root, "root"},
        {Permission::cgroup_write, "cgroup_write"},
        {Permission::signal_process, "signal_process"},
        {Permission::kubernetes_rbac, "kubernetes_rbac"},
    };
    std::string s;
    for (const auto& [p, n] : names) {
        if (!contains(p)) continue;
        if (!s.empty()) s += ",";
        s += n;
    }
    return s.empty() ? "none" : s;
}

std::vector<std::string> validate(const CapabilityDescriptor& d) {
    std::vector<std::string> v;
    const bool id_ok = !d.id.empty() && std::all_of(d.id.begin(), d.id.end(), [](char c) {
        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) ||
               c == '.' || c == '_';
    });
    if (!id_ok) v.emplace_back("id must be non-empty lowercase [a-z0-9._]");
    const std::string_view prefix = d.kind == CapabilityKind::observe ? "observe." : "act.";
    if (d.id.rfind(prefix, 0) != 0) v.emplace_back("id prefix must match kind (" + std::string(prefix) + ")");
    if (d.platforms.empty()) v.emplace_back("must support at least one platform");
    if (d.description.empty()) v.emplace_back("description required");
    if (d.kind == CapabilityKind::observe) {
        if (d.has_side_effects) v.emplace_back("observe capability cannot have side effects");
        if (d.risk > RiskLevel::low) v.emplace_back("observe capability risk must be none/low");
        if (d.observations.empty()) v.emplace_back("observe capability must name observation kinds");
    } else {
        if (!d.has_side_effects) v.emplace_back("act capability must declare side effects");
        if (d.side_effects.empty()) v.emplace_back("act capability must describe side effects");
        if (d.risk >= RiskLevel::medium && !d.requires_confirmation)
            v.emplace_back("medium+ risk requires confirmation");
        if (!d.reversible && d.rollback.empty()) v.emplace_back("irreversible action must document rollback infeasibility");
        if (d.preconditions.empty() || d.postconditions.empty())
            v.emplace_back("act capability must document pre/postconditions");
    }
    return v;
}

Result<void> CapabilityRegistry::add(CapabilityDescriptor d) {
    auto violations = validate(d);
    if (!violations.empty()) {
        std::string msg = d.id + ": ";
        for (const auto& s : violations) msg += s + "; ";
        return make_error(Errc::invalid_argument, msg);
    }
    if (find(d.id) != nullptr) return make_error(Errc::invalid_argument, "duplicate capability " + d.id);
    caps_.push_back(std::move(d));
    return {};
}

const CapabilityDescriptor* CapabilityRegistry::find(std::string_view id) const {
    for (const auto& c : caps_)
        if (c.id == id) return &c;
    return nullptr;
}

const CapabilityDescriptor* CapabilityRegistry::for_observation(ObservationKind k) const {
    for (const auto& c : caps_) {
        if (c.kind != CapabilityKind::observe) continue;
        if (std::find(c.observations.begin(), c.observations.end(), k) != c.observations.end()) return &c;
    }
    return nullptr;
}

namespace {

CapabilityDescriptor observe(std::string id, std::string desc, PlatformSet ps, std::vector<ObservationKind> kinds,
                             std::string pre = "none") {
    CapabilityDescriptor d;
    d.id = std::move(id);
    d.description = std::move(desc);
    d.kind = CapabilityKind::observe;
    d.risk = RiskLevel::none;
    d.platforms = ps;
    d.required_permissions = {Permission::read_own_processes};
    d.observations = std::move(kinds);
    d.preconditions = std::move(pre);
    d.postconditions = "returns a timestamped Observation with provenance; no system state changes";
    d.side_effects = "none";
    d.rollback = "not applicable";
    d.implemented = true;
    return d;
}

CapabilityDescriptor act(std::string id, std::string desc, RiskLevel risk, PlatformSet ps, PermissionSet perms,
                         bool reversible, std::string effects, std::string pre, std::string post,
                         std::string rollback) {
    CapabilityDescriptor d;
    d.id = std::move(id);
    d.description = std::move(desc);
    d.kind = CapabilityKind::act;
    d.risk = risk;
    d.platforms = ps;
    d.required_permissions = perms;
    d.requires_confirmation = true;
    d.reversible = reversible;
    d.has_side_effects = true;
    d.side_effects = std::move(effects);
    d.preconditions = std::move(pre);
    d.postconditions = std::move(post);
    d.rollback = std::move(rollback);
    d.implemented = false;  // V1: no executor exists
    return d;
}

}  // namespace

CapabilityRegistry CapabilityRegistry::builtin() {
    using K = ObservationKind;
    const auto all = PlatformSet::all();
    const PlatformSet mac{Platform::macos};
    const PlatformSet lin{Platform::linux_os};
    CapabilityRegistry r;
    auto must = [&r](CapabilityDescriptor d) {
        auto res = r.add(std::move(d));
        if (!res) throw std::logic_error("builtin capability invalid: " + res.error().message);
    };
    must(observe("observe.system.info", "OS, kernel, hardware model and memory size", all, {K::system_info}));
    must(observe("observe.cpu.topology", "physical/logical CPUs, clusters, caches", all, {K::cpu_topology}));
    must(observe("observe.cpu.times", "cumulative per-CPU tick counters for utilisation", all, {K::cpu_times}));
    must(observe("observe.cpu.load_average", "1/5/15 minute scheduler load averages", all, {K::load_average}));
    must(observe("observe.process.list", "enumerate processes with identity, CPU time and memory", all,
                 {K::process_list}, "processes of other users are visible only partially without privileges"));
    must(observe("observe.process.detail", "inspect one process by PID", all, {K::process_detail},
                 "PID must exist; identity re-checked by start time"));
    must(observe("observe.memory.vm", "virtual-memory statistics and paging counters", all, {K::memory}));
    must(observe("observe.memory.pressure_level", "kernel memory-pressure level", mac, {K::memory_pressure_level}));
    must(observe("observe.memory.swap", "swap usage", all, {K::swap}));
    must(observe("observe.storage.filesystems", "mounted filesystems and capacity", all, {K::filesystems}));
    must(observe("observe.network.interfaces", "interface state and cumulative counters", all,
                 {K::network_interfaces}));
    must(observe("observe.power.thermal_state", "NSProcessInfo thermal state and low-power mode", mac,
                 {K::thermal_state}));
    must(observe("observe.pressure.psi", "pressure stall information (cpu/memory/io)", lin, {K::pressure_stall},
                 "kernel built with CONFIG_PSI and not booted with psi=0"));
    must(observe("observe.cgroup.v2", "cgroup v2 cpu/memory/pids controller state", lin, {K::cgroup},
                 "unified cgroup hierarchy mounted"));

    // Declared but disabled actions. They exist so requests can be mapped to a
    // concrete, explainable refusal; there is no executor in V1.
    must(act("act.process.terminate", "send a termination signal to a process", RiskLevel::high, all,
             {Permission::signal_process}, false, "process stops; unsaved data may be lost",
             "target identity (pid+start time) verified; not a system-critical process",
             "process no longer exists", "infeasible: a terminated process cannot be restored"));
    must(act("act.process.set_priority", "change nice value / scheduling priority", RiskLevel::medium, all,
             {Permission::signal_process}, true, "changes scheduler share of the process",
             "target identity verified; new value within allowed range", "priority equals requested value",
             "restore previous nice value"));
    must(act("act.process.set_affinity", "change CPU affinity", RiskLevel::medium, lin,
             {Permission::signal_process}, true, "restricts the CPUs a task may run on",
             "target identity verified; CPU set online", "affinity mask equals requested set",
             "restore previous mask"));
    must(act("act.kernel.set_parameter", "write a sysctl / kernel tunable", RiskLevel::critical, all,
             {Permission::root}, true, "system-wide kernel behaviour change", "parameter exists and value validated",
             "parameter reads back requested value", "write previous value back (not always safe under load)"));
    must(act("act.cgroup.set_cpu_limit", "change cgroup cpu.max", RiskLevel::high, lin,
             {Permission::cgroup_write}, true, "changes CPU bandwidth of every task in the cgroup",
             "cgroup exists; caller owns it", "cpu.max equals requested quota/period", "restore previous cpu.max"));
    must(act("act.service.restart", "restart a managed service", RiskLevel::high, all, {Permission::root}, false,
             "service downtime; in-flight requests dropped", "service exists and is managed",
             "service active again", "infeasible: in-flight work is lost"));
    must(act("act.network.configure", "change network configuration", RiskLevel::critical, all,
             {Permission::root}, true, "connectivity may be lost", "interface exists",
             "configuration applied", "restore previous configuration if still reachable"));
    must(act("act.kubernetes.patch", "modify Kubernetes resources", RiskLevel::critical, lin,
             {Permission::kubernetes_rbac}, true, "workload rescheduling / restarts", "RBAC allows patch",
             "resource version updated", "re-apply previous manifest"));
    return r;
}

// ---- policy & audit ------------------------------------------------------

PolicyDecision SafetyPolicy::authorize(const CapabilityDescriptor& d, Platform platform,
                                       const ObservationSet& supported) const {
    PolicyDecision dec;
    dec.capability = d.id;
    dec.at = std::chrono::system_clock::now();
    if (d.kind == CapabilityKind::act || d.has_side_effects) {
        dec.reason = "V1 safety policy is read-only: actions with side effects are disabled";
        return dec;
    }
    if (!d.platforms.contains(platform)) {
        dec.reason = "capability not available on " + std::string(to_string(platform));
        return dec;
    }
    if (d.required_permissions.privileged() && !cfg_.allow_privileged_reads) {
        dec.reason = "capability requires privileges OSIE does not request";
        return dec;
    }
    for (auto k : d.observations) {
        if (!supported.contains(k)) {
            dec.reason = "observation '" + std::string(to_string(k)) + "' unsupported by this kernel/adapter";
            return dec;
        }
    }
    if (!d.implemented) {
        dec.reason = "no executor implemented";
        return dec;
    }
    dec.allowed = true;
    dec.reason = "read-only observation";
    return dec;
}

Result<void> AuditLog::open_file(const std::string& path) {
    std::lock_guard lk(mu_);
    file_.open(path, std::ios::app);
    if (!file_) return make_error(Errc::io_error, "cannot open audit log " + path);
    return {};
}

void AuditLog::record(const PolicyDecision& d) {
    std::lock_guard lk(mu_);
    if (!d.allowed) ++denied_;
    entries_.push_back(d);
    while (entries_.size() > capacity_) entries_.pop_front();
    if (file_.is_open()) {
        json::Writer w;
        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(d.at.time_since_epoch()).count();
        w.begin_object().kv("ts_ms", static_cast<std::int64_t>(ts)).kv("capability", d.capability)
            .kv("allowed", d.allowed).kv("reason", d.reason).end_object();
        file_ << w.str() << '\n';
        file_.flush();
    }
}

std::vector<PolicyDecision> AuditLog::recent(std::size_t n) const {
    std::lock_guard lk(mu_);
    const std::size_t start = entries_.size() > n ? entries_.size() - n : 0;
    return {entries_.begin() + static_cast<std::ptrdiff_t>(start), entries_.end()};
}

std::size_t AuditLog::denied_count() const {
    std::lock_guard lk(mu_);
    return denied_;
}

// ---- broker --------------------------------------------------------------

Result<void> ObservationBroker::authorize(ObservationKind k) {
    const auto* cap = reg_.for_observation(k);
    if (cap == nullptr) {
        PolicyDecision d{"observe.<unregistered:" + std::string(to_string(k)) + ">", false,
                         "no registered capability", std::chrono::system_clock::now()};
        audit_.record(d);
        return make_error(Errc::policy_denied, d.reason);
    }
    auto d = pol_.authorize(*cap, os_.platform(), os_.supported());
    audit_.record(d);
    if (!d.allowed) {
        // Distinguish "the platform cannot" from "policy will not".
        const bool unsupported = !cap->platforms.contains(os_.platform()) || !os_.supported().contains(k);
        return make_error(unsupported ? Errc::unsupported : Errc::policy_denied, cap->id + ": " + d.reason);
    }
    return {};
}

#define OSIE_BROKER_GUARD(kind)          \
    if (auto a = authorize(kind); !a) {  \
        return a.error();                \
    }

Result<Observation<SystemInfo>> ObservationBroker::system_info() {
    OSIE_BROKER_GUARD(ObservationKind::system_info)
    return os_.system_info();
}
Result<Observation<CpuTopology>> ObservationBroker::cpu_topology() {
    OSIE_BROKER_GUARD(ObservationKind::cpu_topology)
    return os_.cpu_topology();
}
Result<Observation<CpuSample>> ObservationBroker::cpu_sample() {
    OSIE_BROKER_GUARD(ObservationKind::cpu_times)
    return os_.cpu_sample();
}
Result<Observation<MemoryMetrics>> ObservationBroker::memory() {
    OSIE_BROKER_GUARD(ObservationKind::memory)
    return os_.memory();
}
Result<Observation<ProcessSnapshot>> ObservationBroker::processes(const ProcessQuery& q, const CancellationToken& c) {
    OSIE_BROKER_GUARD(ObservationKind::process_list)
    return os_.processes(q, c);
}
Result<Observation<ProcessInfo>> ObservationBroker::process(Pid pid) {
    OSIE_BROKER_GUARD(ObservationKind::process_detail)
    return os_.process(pid);
}
Result<Observation<std::vector<FilesystemInfo>>> ObservationBroker::filesystems() {
    OSIE_BROKER_GUARD(ObservationKind::filesystems)
    return os_.filesystems();
}
Result<Observation<std::vector<NetInterface>>> ObservationBroker::network_interfaces() {
    OSIE_BROKER_GUARD(ObservationKind::network_interfaces)
    return os_.network_interfaces();
}
Result<Observation<MacPowerState>> ObservationBroker::power_state() {
    OSIE_BROKER_GUARD(ObservationKind::thermal_state)
    auto* mac = os_.macos();
    if (mac == nullptr) return make_error(Errc::unsupported, "thermal state is macOS-only");
    return mac->power_state();
}
Result<Observation<std::vector<PressureStall>>> ObservationBroker::pressure() {
    OSIE_BROKER_GUARD(ObservationKind::pressure_stall)
    auto* lx = os_.linux_ext();
    if (lx == nullptr) return make_error(Errc::unsupported, "PSI is Linux-only");
    return lx->pressure();
}
Result<Observation<CgroupInfo>> ObservationBroker::cgroup(const std::string& path) {
    OSIE_BROKER_GUARD(ObservationKind::cgroup)
    auto* lx = os_.linux_ext();
    if (lx == nullptr) return make_error(Errc::unsupported, "cgroup v2 is Linux-only");
    return lx->cgroup(path);
}
Result<std::string> ObservationBroker::cgroup_of(Pid pid) {
    OSIE_BROKER_GUARD(ObservationKind::cgroup)
    auto* lx = os_.linux_ext();
    if (lx == nullptr) return make_error(Errc::unsupported, "cgroup v2 is Linux-only");
    return lx->cgroup_of(pid);
}

#undef OSIE_BROKER_GUARD

}  // namespace osie
