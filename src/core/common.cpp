// SPDX-License-Identifier: Apache-2.0
#include <cerrno>

#include "osie/core/observation.hpp"
#include "osie/core/result.hpp"
#include "osie/platform/os.hpp"

namespace osie {

Errc errc_from_errno(int err) noexcept {
    switch (err) {
        case EPERM:
        case EACCES: return Errc::permission_denied;
        case ESRCH: return Errc::process_exited;
        case ENOENT: return Errc::not_found;
        case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
#endif
        case ENOSYS: return Errc::unsupported;
        case EINVAL: return Errc::invalid_argument;
        case EINTR: return Errc::cancelled;
        default: return Errc::io_error;
    }
}

Platform host_platform() noexcept {
#if defined(__APPLE__)
    return Platform::macos;
#else
    return Platform::linux_os;
#endif
}

std::string PlatformSet::to_string() const {
    std::string s;
    for (auto p : {Platform::macos, Platform::linux_os}) {
        if (!contains(p)) continue;
        if (!s.empty()) s += ",";
        s += osie::to_string(p);
    }
    return s.empty() ? "none" : s;
}

std::string_view to_string(ObservationKind k) noexcept {
    switch (k) {
        case ObservationKind::system_info: return "system_info";
        case ObservationKind::cpu_topology: return "cpu_topology";
        case ObservationKind::cpu_times: return "cpu_times";
        case ObservationKind::load_average: return "load_average";
        case ObservationKind::process_list: return "process_list";
        case ObservationKind::process_detail: return "process_detail";
        case ObservationKind::memory: return "memory";
        case ObservationKind::memory_pressure_level: return "memory_pressure_level";
        case ObservationKind::swap: return "swap";
        case ObservationKind::filesystems: return "filesystems";
        case ObservationKind::network_interfaces: return "network_interfaces";
        case ObservationKind::thermal_state: return "thermal_state";
        case ObservationKind::pressure_stall: return "pressure_stall";
        case ObservationKind::cgroup: return "cgroup";
        case ObservationKind::count_: break;
    }
    return "?";
}

std::vector<ObservationKind> ObservationSet::list() const {
    std::vector<ObservationKind> out;
    for (unsigned i = 0; i < static_cast<unsigned>(ObservationKind::count_); ++i) {
        auto k = static_cast<ObservationKind>(i);
        if (contains(k)) out.push_back(k);
    }
    return out;
}

std::string_view to_string(LimitStatus s) noexcept {
    switch (s) {
        case LimitStatus::known: return "known";
        case LimitStatus::unlimited: return "unlimited";
        case LimitStatus::unknown: return "unknown";
        case LimitStatus::not_applicable: return "not_applicable";
    }
    return "?";
}

CpuCapacity finalize_capacity(CpuCapacity c) {
    c.effective_cpus.reset();
    c.binding.clear();
    c.unknown_limits.clear();
    const std::pair<const char*, const CpuLimit*> limits[] = {{"host logical CPUs", &c.host_logical},
                                                              {"online CPUs", &c.online},
                                                              {"CPU affinity", &c.affinity},
                                                              {"cgroup cpuset", &c.cpuset},
                                                              {"cgroup CPU quota", &c.quota}};
    for (const auto& [name, l] : limits) {
        if (l->status == LimitStatus::unknown) {
            c.unknown_limits.emplace_back(name);
            continue;
        }
        if (l->status != LimitStatus::known || !l->cpus || !(*l->cpus > 0)) continue;
        // Ties keep the earlier (more fundamental) limit as the binding one.
        if (!c.effective_cpus || *l->cpus < *c.effective_cpus - 1e-9) {
            c.effective_cpus = *l->cpus;
            c.binding = name;
        }
    }
    return c;
}

}  // namespace osie
