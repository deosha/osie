// SPDX-License-Identifier: Apache-2.0
// Linux: cgroup v2 controllers and Pressure Stall Information.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sched.h>
#include <unistd.h>

#include "linux_os.hpp"
#include "osie/platform/cpu_list.hpp"
#include "osie/architecture/model.hpp"

namespace osie::linux_os {

Result<Observation<std::vector<PressureStall>>> LinuxOperatingSystem::pressure() {
    if (!supported_.contains(ObservationKind::pressure_stall))
        return make_error(Errc::unsupported, "PSI not available (CONFIG_PSI missing or psi=0)");
    auto prov = Provenance::now("procfs:/proc/pressure/{cpu,memory,io}");
    std::vector<PressureStall> out;
    for (auto [file, res] : {std::pair{"/proc/pressure/cpu", PsiResource::cpu},
                             std::pair{"/proc/pressure/memory", PsiResource::memory},
                             std::pair{"/proc/pressure/io", PsiResource::io}}) {
        auto body = read_file(p(file));
        if (!body) continue;
        if (auto ps = parse_psi(*body, res)) out.push_back(*ps);
    }
    if (out.empty()) return make_error(Errc::unavailable, "PSI files unreadable");
    if (out.size() < 3) prov.validity = Validity::partial;
    return Observation<std::vector<PressureStall>>{std::move(out), std::move(prov)};
}

Result<std::string> LinuxOperatingSystem::cgroup_of(Pid pid) {
    if (pid <= 0) return make_error(Errc::invalid_argument, "pid must be positive");
    auto body = read_file(p("/proc/" + std::to_string(pid) + "/cgroup"), 4096);
    if (!body) {
        if (body.error().code == Errc::not_found) return make_error(Errc::process_exited, "pid " + std::to_string(pid));
        return body.error();
    }
    const auto pos = body->find("0::");
    if (pos == std::string::npos) return make_error(Errc::unsupported, "process is not in a cgroup v2 hierarchy");
    const auto eol = body->find('\n', pos);
    return body->substr(pos + 3, eol == std::string::npos ? std::string::npos : eol - pos - 3);
}

Result<Observation<CgroupInfo>> LinuxOperatingSystem::cgroup(const std::string& path) {
    if (!supported_.contains(ObservationKind::cgroup)) return make_error(Errc::unsupported, "no cgroup v2 hierarchy");
    // Paths come from /proc/<pid>/cgroup, but validate anyway: no traversal.
    if (path.empty() || path.front() != '/' || path.find("..") != std::string::npos || path.find('\0') != std::string::npos)
        return make_error(Errc::invalid_argument, "invalid cgroup path");
    auto prov = Provenance::now("cgroupfs:cpu.max,cpu.stat,memory.*,pids.current,*.pressure");
    const std::string dir = root_ + cgroup_mount_ + (path == "/" ? "" : path) + "/";
    auto stat = read_file(dir + "cpu.stat");
    if (!stat) {
        if (stat.error().code == Errc::not_found) return make_error(Errc::not_found, "cgroup " + path + " not visible");
        return stat.error();
    }
    CgroupInfo ci;
    ci.path = path;
    ci.usage_usec = space_field(*stat, "usage_usec");
    ci.user_usec = space_field(*stat, "user_usec");
    ci.system_usec = space_field(*stat, "system_usec");
    ci.nr_periods = space_field(*stat, "nr_periods");  // present only with the cpu controller enabled
    ci.nr_throttled = space_field(*stat, "nr_throttled");
    ci.throttled_usec = space_field(*stat, "throttled_usec");
    if (auto mx = read_file(dir + "cpu.max", 128)) {
        // "max 100000" (unlimited) or "<quota> <period>"
        char quota[32] = {};
        unsigned long long period = 0;
        if (std::sscanf(mx->c_str(), "%31s %llu", quota, &period) == 2) {
            ci.cpu_period_us = period;
            if (std::string(quota) != "max") ci.cpu_quota_us = std::strtoll(quota, nullptr, 10);
        }
    }
    ci.cpu_weight = read_u64(dir + "cpu.weight");
    ci.memory_current = read_u64(dir + "memory.current");
    ci.memory_max = read_u64(dir + "memory.max");    // "max" -> nullopt (unlimited)
    ci.memory_high = read_u64(dir + "memory.high");
    ci.pids_current = read_u64(dir + "pids.current");
    if (auto cs = read_file(dir + "cpuset.cpus.effective", 4096)) ci.cpuset_cpus = parse_cpu_list_count(*cs);
    for (auto [file, res] : {std::pair{"cpu.pressure", PsiResource::cpu}, std::pair{"memory.pressure", PsiResource::memory},
                             std::pair{"io.pressure", PsiResource::io}}) {
        if (auto b = read_file(dir + file, 512))
            if (auto ps = parse_psi(*b, res)) ci.pressure.push_back(*ps);
    }
    ci.systemd_unit = arch::systemd_unit_from_cgroup(path);
    if (!ci.nr_periods) prov.note = "cpu controller not enabled for this cgroup (no bandwidth statistics)";
    return Observation<CgroupInfo>{std::move(ci), std::move(prov)};
}

Result<Observation<CpuCapacity>> LinuxOperatingSystem::cpu_capacity(Pid pid) {
    if (pid < 0) return make_error(Errc::invalid_argument, "pid must be non-negative");
    auto prov = Provenance::now("sysfs+sched_getaffinity+cgroupfs:cpuset.cpus.effective,cpu.max");
    CpuCapacity c;
    c.scope = pid == 0 ? "this process" : "pid " + std::to_string(pid);

    const long conf = ::sysconf(_SC_NPROCESSORS_CONF);
    if (conf > 0) c.host_logical = CpuLimit{LimitStatus::known, static_cast<double>(conf), "sysconf(_SC_NPROCESSORS_CONF)", ""};
    else c.host_logical = CpuLimit{LimitStatus::unknown, std::nullopt, "sysconf(_SC_NPROCESSORS_CONF)", "sysconf failed"};
    if (auto on = read_file(p("/sys/devices/system/cpu/online"), 4096)) {
        const auto n = parse_cpu_list(*on).size();
        c.online = n > 0 ? CpuLimit{LimitStatus::known, static_cast<double>(n), "/sys/devices/system/cpu/online", ""}
                         : CpuLimit{LimitStatus::unknown, std::nullopt, "/sys/devices/system/cpu/online", "unparseable"};
    } else {
        c.online = CpuLimit{LimitStatus::unknown, std::nullopt, "/sys/devices/system/cpu/online", on.error().describe()};
    }

    // Affinity: a dynamically sized mask, so hosts with more than
    // CPU_SETSIZE CPUs are counted correctly.
    {
        const int ncpu = conf > 0 ? static_cast<int>(std::max<long>(conf, 1024)) : 1024;
        cpu_set_t* set = CPU_ALLOC(static_cast<std::size_t>(ncpu));
        const std::size_t sz = CPU_ALLOC_SIZE(static_cast<std::size_t>(ncpu));
        if (set != nullptr) {
            CPU_ZERO_S(sz, set);
            if (::sched_getaffinity(pid, sz, set) == 0) {
                c.affinity = CpuLimit{LimitStatus::known, static_cast<double>(CPU_COUNT_S(sz, set)), "sched_getaffinity(2)", ""};
            } else {
                const int err = errno;
                c.affinity = CpuLimit{LimitStatus::unknown, std::nullopt, "sched_getaffinity(2)",
                                      err == ESRCH ? "process exited" : err == EPERM ? "permission denied" : std::strerror(err)};
            }
            CPU_FREE(set);
        }
    }

    // cgroup v2: cpuset of the leaf, quota as the minimum over the leaf and
    // every visible ancestor (bandwidth limits are hierarchical).
    if (!supported_.contains(ObservationKind::cgroup)) {
        c.cpuset = CpuLimit{LimitStatus::unknown, std::nullopt, "cgroupfs", "no cgroup v2 hierarchy (cgroup v1 is not supported)"};
        c.quota = c.cpuset;
    } else if (auto cg = cgroup_of(pid == 0 ? ::getpid() : pid); !cg) {
        c.cpuset = CpuLimit{LimitStatus::unknown, std::nullopt, "/proc/<pid>/cgroup", cg.error().describe()};
        c.quota = c.cpuset;
    } else {
        const std::string leaf = *cg;
        const std::string base = root_ + cgroup_mount_;
        if (auto cs = read_file(base + (leaf == "/" ? "" : leaf) + "/cpuset.cpus.effective", 4096)) {
            const auto n = parse_cpu_list(*cs).size();
            c.cpuset = n > 0 ? CpuLimit{LimitStatus::known, static_cast<double>(n), "cpuset.cpus.effective of " + leaf, ""}
                             : CpuLimit{LimitStatus::unknown, std::nullopt, "cpuset.cpus.effective of " + leaf, "empty"};
        } else {
            // Without the cpuset controller the cgroup imposes no cpuset;
            // the affinity mask still reflects any inherited restriction.
            c.cpuset = CpuLimit{LimitStatus::unlimited, std::nullopt, "cpuset.cpus.effective of " + leaf,
                                "cpuset controller not enabled for this cgroup"};
        }
        std::optional<double> min_quota;
        bool any_read = false;
        std::string path = leaf;
        for (int depth = 0; depth < 64; ++depth) {
            const std::string dir = base + (path == "/" ? "" : path);
            if (auto mx = read_file(dir + "/cpu.max", 128)) {
                any_read = true;
                char q[32] = {};
                unsigned long long period = 0;
                if (std::sscanf(mx->c_str(), "%31s %llu", q, &period) == 2 && period > 0) {
                    if (std::string(q) == "max") {
                        c.quota_chain.push_back(path + ": max");
                    } else {
                        const double cores = static_cast<double>(std::strtoll(q, nullptr, 10)) / static_cast<double>(period);
                        c.quota_chain.push_back(path + ": " + std::to_string(cores).substr(0, 5) + " CPUs");
                        if (cores > 0 && (!min_quota || cores < *min_quota)) min_quota = cores;
                    }
                }
            }
            if (path == "/" || path.empty()) break;
            const auto slash = path.rfind('/');
            path = slash == 0 ? "/" : path.substr(0, slash);
        }
        if (min_quota) c.quota = CpuLimit{LimitStatus::known, *min_quota, "cpu.max of " + leaf + " and ancestors", ""};
        else if (any_read) c.quota = CpuLimit{LimitStatus::unlimited, std::nullopt, "cpu.max of " + leaf + " and ancestors", ""};
        else c.quota = CpuLimit{LimitStatus::unknown, std::nullopt, "cpu.max", "cpu controller not enabled or not visible"};
        // Inside a cgroup namespace the ancestors above the namespace root are
        // not visible, so a tighter limit there cannot be ruled out.
        if (leaf == "/") c.quota.note = "cgroup namespace root: limits above it are not visible from here";
    }
    return Observation<CpuCapacity>{finalize_capacity(std::move(c)), std::move(prov)};
}

}  // namespace osie::linux_os

namespace osie {
std::unique_ptr<IOperatingSystem> make_native_os() { return std::make_unique<linux_os::LinuxOperatingSystem>(); }
}  // namespace osie
