// SPDX-License-Identifier: Apache-2.0
#include "osie/architecture/model.hpp"

#include <pwd.h>
#include <unistd.h>

#include <array>
#include <unordered_map>

namespace osie::arch {
namespace ck = concept_key;

namespace {

struct ConceptDef {
    std::string_view key;
    std::string_view label;
    PlatformSet platforms;
    std::string_view doc;
};

struct RelationDef {
    std::string_view from;
    EdgeKind kind;
    std::string_view to;
    EdgeNature nature;
    float confidence;
    PlatformSet platforms;
    std::string_view doc;
    std::string_view mechanism;
};

constexpr PlatformSet kAll{Platform::macos, Platform::linux_os};
constexpr PlatformSet kMac{Platform::macos};
constexpr PlatformSet kLinux{Platform::linux_os};

// Documentation references are cited by title so they remain meaningful offline.
constexpr std::string_view kDocSchedLinux = "Linux: Documentation/scheduler/sched-design-CFS.rst, sched-eevdf.rst";
constexpr std::string_view kDocBwc = "Linux: Documentation/scheduler/sched-bwc.rst (CFS bandwidth control)";
constexpr std::string_view kDocCgroup = "Linux: Documentation/admin-guide/cgroup-v2.rst";
constexpr std::string_view kDocPsi = "Linux: Documentation/accounting/psi.rst";
constexpr std::string_view kDocWorld = "docs/WORLD_MODEL_RESULTS.md (randomised interventions; Linux 6.10 arm64 Docker VM only)";
constexpr std::string_view kDocProcfs = "Linux: proc(5) /proc/loadavg, /proc/stat, /proc/vmstat";
constexpr std::string_view kDocMm = "Linux: Documentation/admin-guide/mm/concepts.rst";
constexpr std::string_view kDocXnuVm = "Apple: Kernel Programming Guide, Memory and Virtual Memory; xnu osfmk/vm";
constexpr std::string_view kDocActMon = "Apple: Activity Monitor User Guide, 'View memory usage' (memory pressure, compressed)";
constexpr std::string_view kDocThermal = "Apple: NSProcessInfo.thermalState / NSProcessInfoThermalState documentation";
constexpr std::string_view kDocQos = "Apple: Energy Efficiency Guide for Mac Apps, 'Prioritize Work with Quality of Service Classes'";
constexpr std::string_view kDocAs = "Apple: 'Tuning your code's performance for Apple silicon' (P- and E-cores)";
constexpr std::string_view kDocSignal = "POSIX: signal(7), kill(2); Linux: mm/oom_kill.c";
constexpr std::string_view kDocQueueing = "Queueing theory / USE method (B. Gregg): utilisation vs saturation";

constexpr std::array<ConceptDef, 33> kConcepts{{
    {ck::cpu, "CPU subsystem", kAll, kDocQueueing},
    {ck::scheduler, "CPU scheduler", kAll, kDocSchedLinux},
    {ck::run_queue, "Run queue (runnable threads waiting for a CPU)", kAll, kDocSchedLinux},
    {ck::cpu_saturation, "CPU saturation (demand exceeds CPUs)", kAll, kDocQueueing},
    {ck::scheduling_latency, "Scheduling latency (time runnable but not running)", kAll, kDocPsi},
    {ck::load_average, "Load average", kAll, kDocProcfs},
    {ck::heterogeneous_cores, "Heterogeneous cores (performance/efficiency)", kMac, kDocAs},
    {ck::qos_classes, "Quality-of-service classes", kMac, kDocQos},
    {ck::thermal_pressure, "Thermal pressure", kMac, kDocThermal},
    {ck::cpu_frequency, "Effective CPU frequency/performance", kAll, kDocThermal},
    {ck::cpu_quota, "cgroup CPU bandwidth quota (cpu.max)", kLinux, kDocBwc},
    {ck::cpu_throttling, "CPU bandwidth throttling", kLinux, kDocBwc},
    {ck::psi_cpu, "PSI cpu (time tasks wait for CPU)", kLinux, kDocPsi},
    {ck::hypervisor_steal, "Hypervisor steal time", kLinux, kDocProcfs},
    {ck::memory, "Memory subsystem", kAll, kDocMm},
    {ck::physical_memory, "Physical memory (RAM)", kAll, kDocMm},
    {ck::virtual_memory, "Virtual memory", kAll, kDocXnuVm},
    {ck::page_cache, "Page cache / file-backed pages", kAll, kDocMm},
    {ck::memory_pressure, "Memory pressure", kAll, kDocActMon},
    {ck::memory_compression, "Memory compressor", kMac, kDocActMon},
    {ck::paging, "Paging (page-outs/page-ins)", kAll, kDocXnuVm},
    {ck::swapping, "Swapping to disk", kAll, kDocMm},
    {ck::major_faults, "Major page faults (faults needing I/O)", kAll, kDocMm},
    {ck::oom, "Out-of-memory termination (Linux OOM killer / macOS jetsam)", kAll, kDocCgroup},
    {ck::priority, "Process priority (nice value): a scheduling weight; lower nice gets a larger CPU share under contention", kAll, kDocSchedLinux},
    {ck::signals, "Process signals (SIGTERM asks a process to exit and lets it clean up; SIGKILL, as in kill -9, ends it immediately and cannot be caught)", kAll, kDocSignal},
    {ck::vm_pressure_level, "Kernel VM pressure level", kMac, kDocActMon},
    {ck::psi_memory, "PSI memory", kLinux, kDocPsi},
    {ck::memory_limit, "cgroup memory limit (memory.high/max)", kLinux, kDocCgroup},
    {ck::disk_io, "Disk I/O", kAll, kDocMm},
    {ck::io_latency, "I/O latency", kAll, kDocPsi},
    {ck::psi_io, "PSI io", kLinux, kDocPsi},
    {ck::responsiveness, "Application responsiveness (outcome)", kAll, kDocQueueing},
}};

using N = EdgeNature;
using E = EdgeKind;

const std::vector<RelationDef>& relations() {
    static const std::vector<RelationDef> r = {
        // structure (invariants)
        {ck::scheduler, E::part_of, ck::cpu, N::invariant, 1.0F, kAll, kDocSchedLinux, "the scheduler multiplexes threads onto CPUs"},
        {ck::run_queue, E::part_of, ck::scheduler, N::invariant, 1.0F, kAll, kDocSchedLinux, "runnable threads wait in per-CPU run queues"},
        {ck::heterogeneous_cores, E::part_of, ck::cpu, N::invariant, 1.0F, kMac, kDocAs, "Apple silicon has performance and efficiency clusters"},
        {ck::physical_memory, E::part_of, ck::memory, N::invariant, 1.0F, kAll, kDocMm, ""},
        {ck::virtual_memory, E::part_of, ck::memory, N::invariant, 1.0F, kAll, kDocXnuVm, "each process sees a private virtual address space backed by RAM, compressor or disk"},
        {ck::page_cache, E::part_of, ck::physical_memory, N::invariant, 1.0F, kAll, kDocMm, "file data is cached in otherwise unused RAM"},
        {ck::memory_compression, E::part_of, ck::virtual_memory, N::invariant, 1.0F, kMac, kDocActMon, "macOS compresses inactive anonymous pages before swapping"},
        {ck::swapping, E::part_of, ck::paging, N::invariant, 1.0F, kAll, kDocMm, ""},
        {ck::cpu_quota, E::part_of, ck::scheduler, N::invariant, 1.0F, kLinux, kDocBwc, "CFS/EEVDF enforce cgroup bandwidth per period"},
        // measurement relations
        {ck::run_queue, E::measured_by, ck::load_average, N::invariant, 1.0F, kAll, kDocProcfs, "load average is an exponentially-damped count of runnable (Linux: also uninterruptible) tasks"},
        {ck::cpu_saturation, E::measured_by, ck::psi_cpu, N::invariant, 1.0F, kLinux, kDocPsi, "PSI cpu 'some' is the share of time at least one task waited for a CPU"},
        {ck::memory_pressure, E::measured_by, ck::psi_memory, N::invariant, 1.0F, kLinux, kDocPsi, "PSI memory measures time lost to reclaim, refaults and swap-in"},
        {ck::memory_pressure, E::measured_by, ck::vm_pressure_level, N::invariant, 1.0F, kMac, kDocActMon, "XNU publishes a normal/warning/critical pressure level"},
        {ck::io_latency, E::measured_by, ck::psi_io, N::invariant, 1.0F, kLinux, kDocPsi, ""},
        // documented mechanisms (invariants where the platform defines the behaviour)
        {ck::cpu_quota, E::affects, ck::cpu_throttling, N::invariant, 1.0F, kLinux, kDocBwc, "a cgroup that exhausts its cpu.max quota in a period is throttled until the next period"},
        {ck::cpu_throttling, E::affects, ck::scheduling_latency, N::invariant, 1.0F, kLinux, kDocBwc, "throttled tasks are not scheduled even when CPUs are idle"},
        // Effects measured under randomised interventions in the OSIE lab.
        // Their size is environment-specific; their direction matches the invariants above.
        {ck::cpu_quota, E::affects, ck::psi_cpu, N::experimental, 0.95F, kLinux, kDocWorld, "lowering a cgroup's CPU quota raised its CPU PSI some by 23 pp (95% CI 18-29) over 163 randomised changes; raising it lowered PSI by 8 pp (5-11)"},
        {ck::cpu_throttling, E::affects, ck::responsiveness, N::experimental, 0.95F, kLinux, kDocWorld, "halving the quota from 1 to 0.5 core raised p99 latency of instrumented request servers 15x (geometric mean, 95% CI 8-29x, 30 configurations)"},
        {ck::memory_pressure, E::affects, ck::memory_compression, N::invariant, 1.0F, kMac, kDocActMon, "under pressure macOS compresses memory to free pages"},
        {ck::memory_pressure, E::affects, ck::page_cache, N::invariant, 1.0F, kAll, kDocMm, "reclaim evicts clean file-backed pages first"},
        {ck::memory_pressure, E::affects, ck::paging, N::invariant, 1.0F, kAll, kDocXnuVm, "when free pages run out the kernel pages out (compressor/swap)"},
        {ck::swapping, E::affects, ck::major_faults, N::invariant, 1.0F, kAll, kDocMm, "touching a swapped-out page requires a major fault and disk read"},
        {ck::memory_limit, E::affects, ck::memory_pressure, N::invariant, 1.0F, kLinux, kDocCgroup, "exceeding memory.high throttles and reclaims inside the cgroup"},
        {ck::cpu_saturation, E::affects, ck::run_queue, N::invariant, 1.0F, kAll, kDocQueueing, "when runnable threads exceed CPUs, the excess must queue"},
        // probabilistic causal hypotheses (need evidence before use)
        {ck::run_queue, E::affects, ck::scheduling_latency, N::hypothesized, 0.85F, kAll, kDocQueueing, "longer run queues usually mean longer waits before a thread runs"},
        {ck::scheduling_latency, E::affects, ck::responsiveness, N::hypothesized, 0.7F, kAll, kDocQueueing, "interactive threads that wait for CPU respond later"},
        {ck::thermal_pressure, E::affects, ck::cpu_frequency, N::hypothesized, 0.8F, kMac, kDocThermal, "at 'serious'/'critical' thermal state the system may reduce CPU performance"},
        {ck::cpu_frequency, E::affects, ck::cpu_saturation, N::hypothesized, 0.6F, kAll, kDocThermal, "lower clock speed means the same work occupies CPUs longer"},
        {ck::qos_classes, E::affects, ck::heterogeneous_cores, N::hypothesized, 0.8F, kMac, kDocQos, "background-QoS work is preferentially placed on efficiency cores"},
        {ck::hypervisor_steal, E::affects, ck::scheduling_latency, N::hypothesized, 0.7F, kLinux, kDocProcfs, "steal time is CPU time the hypervisor gave to other guests"},
        {ck::memory_compression, E::affects, ck::cpu_saturation, N::hypothesized, 0.4F, kMac, kDocActMon, "compressing/decompressing pages costs CPU time"},
        {ck::paging, E::affects, ck::disk_io, N::hypothesized, 0.7F, kAll, kDocMm, "page-outs to swap and page-ins generate storage I/O"},
        {ck::page_cache, E::affects, ck::disk_io, N::hypothesized, 0.6F, kAll, kDocMm, "a smaller page cache turns cache hits into disk reads"},
        {ck::major_faults, E::affects, ck::responsiveness, N::hypothesized, 0.7F, kAll, kDocMm, "a thread blocked on a major fault cannot make progress"},
        {ck::priority, E::part_of, ck::scheduler, N::invariant, 1.0F, kAll, kDocSchedLinux, "the scheduler weights runnable threads by priority; nice only matters when CPUs are contended"},
        {ck::oom, E::uses, ck::signals, N::invariant, 1.0F, kLinux, kDocSignal, "the Linux OOM killer ends the selected process with SIGKILL"},
        {ck::memory_pressure, E::affects, ck::oom, N::hypothesized, 0.4F, kAll, kDocCgroup, "sustained critical pressure can end in OOM-kill / jetsam termination"},
        {ck::disk_io, E::affects, ck::io_latency, N::hypothesized, 0.7F, kAll, kDocPsi, "queueing at the device raises per-request latency"},
        {ck::io_latency, E::affects, ck::responsiveness, N::hypothesized, 0.6F, kAll, kDocPsi, "threads blocked on I/O stall the work that depends on them"},
    };
    return r;
}

std::string user_name(Uid uid) {
    struct passwd pw {};
    struct passwd* res = nullptr;
    std::array<char, 1024> buf{};
    if (::getpwuid_r(uid, &pw, buf.data(), buf.size(), &res) == 0 && res != nullptr) return res->pw_name;
    return "uid " + std::to_string(uid);
}

std::string process_key(const ProcessInfo& p) {
    std::string k = "pid:" + std::to_string(p.pid);
    if (p.start_time_us) k += "@" + std::to_string(*p.start_time_us);
    return k;
}

}  // namespace

void load_ontology(KnowledgeGraph& g) {
    for (const auto& c : kConcepts) {
        NodeId id = g.upsert(NodeKind::concept_, c.key, c.label, "ontology");
        Node& n = g.node_mut(id);
        n.platforms = c.platforms;
        n.doc_ref = c.doc;
    }
    for (const auto& r : relations()) {
        const NodeId a = concept_node(g, r.from);
        const NodeId b = concept_node(g, r.to);
        if (a == kNoNode || b == kNoNode) continue;
        if (auto e = g.link(a, r.kind, b, r.nature, "ontology", r.confidence)) {
            Edge& ed = g.edge_mut(*e);
            ed.platforms = r.platforms;
            ed.doc_ref = r.doc;
            ed.mechanism = r.mechanism;
        }
    }
}

NodeId concept_node(const KnowledgeGraph& g, std::string_view key) {
    const Node* n = g.find(NodeKind::concept_, key);
    return n != nullptr ? n->id : kNoNode;
}

std::optional<std::string> systemd_unit_from_cgroup(std::string_view path) {
    // Deepest component ending in a unit suffix wins ("/user.slice/user-1000.slice/session-2.scope").
    std::optional<std::string> unit;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        auto comp = path.substr(start, end - start);
        auto ends_with = [&](std::string_view suf) {
            return comp.size() > suf.size() && comp.substr(comp.size() - suf.size()) == suf;
        };
        if (ends_with(".service") || ends_with(".scope")) unit = std::string(comp);
        start = end + 1;
    }
    return unit;
}

BuildReport update_instances(KnowledgeGraph& g, const SystemState& s, Platform platform) {
    BuildReport rep;
    g.begin_update();

    std::string host_key = "localhost";
    if (s.system && !s.system->value.hostname.empty()) host_key = s.system->value.hostname;
    const NodeId host = g.upsert(NodeKind::host, host_key, host_key, s.system ? s.system->prov.source : "");
    {
        Node& h = g.node_mut(host);
        h.platforms = PlatformSet{platform};
        if (s.system) {
            h.set_attr("os", s.system->value.os_name + " " + s.system->value.os_version);
            h.set_attr("kernel", s.system->value.kernel_release);
            h.set_attr("model", s.system->value.hardware_model);
            h.set_attr("mem_total_bytes", static_cast<std::int64_t>(s.system->value.mem_total_bytes));
        }
    }

    if (s.topology) {
        for (const auto& c : s.topology->value.clusters) {
            const NodeId cn = g.upsert(NodeKind::cpu_cluster, c.name, c.name + " cluster", s.topology->prov.source);
            Node& n = g.node_mut(cn);
            n.set_attr("physical_cores", static_cast<std::int64_t>(c.physical_cores));
            n.set_attr("logical_cpus", static_cast<std::int64_t>(c.logical_cpus));
            g.link(host, EdgeKind::contains, cn, EdgeNature::observed, s.topology->prov.source);
            g.link(cn, EdgeKind::part_of, concept_node(g, concept_key::cpu), EdgeNature::invariant, "ontology");
        }
        // Note: no public macOS API maps logical CPU indices to perf levels, so
        // CPU->cluster edges are deliberately not asserted.
    }
    if (s.cpu) {
        const auto& per = s.cpu->value.per_cpu_busy_pct;
        for (std::size_t i = 0; i < per.size(); ++i) {
            const NodeId cpu = g.upsert(NodeKind::logical_cpu, "cpu:" + std::to_string(i), "cpu" + std::to_string(i),
                                        s.cpu->prov.source);
            g.node_mut(cpu).set_attr("busy_pct", per[i]);
            g.link(cpu, EdgeKind::part_of, concept_node(g, concept_key::cpu), EdgeNature::invariant, "ontology");
        }
    }

    NodeId mem = kNoNode;
    if (s.memory) {
        const auto& m = s.memory->value;
        mem = g.upsert(NodeKind::memory, "ram", "physical memory", s.memory->prov.source);
        Node& n = g.node_mut(mem);
        n.set_attr("total_bytes", static_cast<std::int64_t>(m.total_bytes));
        if (m.available_bytes) n.set_attr("available_bytes", static_cast<std::int64_t>(*m.available_bytes));
        if (m.compressed_bytes) n.set_attr("compressed_bytes", static_cast<std::int64_t>(*m.compressed_bytes));
        if (m.pressure_level) n.set_attr("pressure_level", std::string(to_string(*m.pressure_level)));
        g.link(host, EdgeKind::contains, mem, EdgeNature::observed, s.memory->prov.source);
        g.link(mem, EdgeKind::part_of, concept_node(g, concept_key::physical_memory), EdgeNature::invariant, "ontology");
        if (m.swap_total_bytes) {
            const NodeId sw = g.upsert(NodeKind::swap, "swap", "swap", s.memory->prov.source);
            g.node_mut(sw).set_attr("used_bytes", static_cast<std::int64_t>(m.swap_used_bytes.value_or(0)));
            g.link(host, EdgeKind::contains, sw, EdgeNature::observed, s.memory->prov.source);
            g.link(mem, EdgeKind::uses, sw, EdgeNature::invariant, "ontology");
        }
    }

    if (s.processes) {
        const auto src = s.processes->prov.source;
        std::unordered_map<Pid, NodeId> by_pid;
        by_pid.reserve(s.processes->value.size());
        std::unordered_map<Uid, NodeId> users;
        for (const auto& pv : s.processes->value) {
            const auto& p = pv.info;
            const NodeId pn = g.upsert(NodeKind::process, process_key(p), p.name, src);
            Node& n = g.node_mut(pn);
            n.set_attr("pid", static_cast<std::int64_t>(p.pid));
            n.set_attr("name", p.name);
            n.set_attr("validity", std::string(to_string(p.validity)));
            if (pv.cpu_pct) n.set_attr("cpu_pct", *pv.cpu_pct);
            if (auto mb = p.memory_bytes()) n.set_attr("memory_bytes", static_cast<std::int64_t>(*mb));
            if (p.threads) n.set_attr("threads", static_cast<std::int64_t>(*p.threads));
            if (platform == Platform::macos && p.ppid == 1) {
                n.set_attr("service_manager", std::string("launchd (job label not exposed by a public API)"));
            }
            by_pid[p.pid] = pn;
            g.link(pn, EdgeKind::part_of, host, EdgeNature::observed, src);
            if (mem != kNoNode && p.memory_bytes()) g.link(pn, EdgeKind::uses, mem, EdgeNature::observed, src);

            auto [uit, inserted] = users.try_emplace(p.uid, kNoNode);
            if (inserted) {
                uit->second = g.upsert(NodeKind::user, "uid:" + std::to_string(p.uid), user_name(p.uid), "getpwuid_r");
                ++rep.users;
            }
            g.link(pn, EdgeKind::owned_by, uit->second, EdgeNature::observed, src);

            if (p.cgroup) {
                const NodeId cg = g.upsert(NodeKind::cgroup, *p.cgroup, *p.cgroup, "procfs:/proc/<pid>/cgroup");
                g.link(pn, EdgeKind::belongs_to, cg, EdgeNature::observed, "procfs:/proc/<pid>/cgroup");
                if (auto unit = systemd_unit_from_cgroup(*p.cgroup)) {
                    const NodeId svc = g.upsert(NodeKind::service, *unit, *unit, "systemd cgroup naming");
                    g.link(svc, EdgeKind::manages, pn, EdgeNature::inferred, "systemd cgroup naming");
                    g.link(svc, EdgeKind::manages, cg, EdgeNature::inferred, "systemd cgroup naming");
                }
            }
            ++rep.processes;
        }
        for (const auto& pv : s.processes->value) {
            const auto& p = pv.info;
            if (p.ppid == p.pid) continue;
            auto parent = by_pid.find(p.ppid);
            if (parent == by_pid.end()) continue;
            // ppid always names a live process (orphans are re-parented), so the
            // edge is an observation, not an inference.
            if (g.link(parent->second, EdgeKind::parent_of, by_pid[p.pid], EdgeNature::observed, src)) ++rep.parent_edges;
        }
        rep.services = g.nodes_of(NodeKind::service).size();
        rep.retired += g.retire_untouched(NodeKind::process);
        rep.retired += g.retire_untouched(NodeKind::user);
        rep.retired += g.retire_untouched(NodeKind::service);
        rep.services = g.nodes_of(NodeKind::service).size();
    }
    for (const auto& cv : s.cgroups) {
        const NodeId cg = g.upsert(NodeKind::cgroup, cv.info.path, cv.info.path, "cgroupfs");
        Node& n = g.node_mut(cg);
        if (cv.info.cpu_quota_us) n.set_attr("cpu_quota_us", *cv.info.cpu_quota_us);
        if (cv.lifetime_throttled_ratio) n.set_attr("lifetime_throttled_ratio", *cv.lifetime_throttled_ratio);
        g.link(host, EdgeKind::contains, cg, EdgeNature::observed, "cgroupfs");
        ++rep.cgroups;
    }
    if (s.processes) rep.retired += g.retire_untouched(NodeKind::cgroup);

    if (s.filesystems) {
        for (const auto& f : s.filesystems->value) {
            const NodeId fn = g.upsert(NodeKind::filesystem, f.mount_point, f.mount_point + " (" + f.fs_type + ")",
                                       s.filesystems->prov.source);
            Node& n = g.node_mut(fn);
            n.set_attr("total_bytes", static_cast<std::int64_t>(f.total_bytes));
            n.set_attr("available_bytes", static_cast<std::int64_t>(f.available_bytes));
            g.link(host, EdgeKind::contains, fn, EdgeNature::observed, s.filesystems->prov.source);
        }
        rep.retired += g.retire_untouched(NodeKind::filesystem);
    }
    if (s.net) {
        for (const auto& ni : s.net->value) {
            const NodeId nn = g.upsert(NodeKind::net_interface, ni.name, ni.name, s.net->prov.source);
            g.node_mut(nn).set_attr("up", ni.up);
            g.link(host, EdgeKind::contains, nn, EdgeNature::observed, s.net->prov.source);
        }
        rep.retired += g.retire_untouched(NodeKind::net_interface);
    }
    return rep;
}

}  // namespace osie::arch
