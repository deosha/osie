// SPDX-License-Identifier: Apache-2.0
// Linux: cgroup v2 controllers and Pressure Stall Information.
#include <cstdlib>

#include "linux_os.hpp"
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
    for (auto [file, res] : {std::pair{"cpu.pressure", PsiResource::cpu}, std::pair{"memory.pressure", PsiResource::memory},
                             std::pair{"io.pressure", PsiResource::io}}) {
        if (auto b = read_file(dir + file, 512))
            if (auto ps = parse_psi(*b, res)) ci.pressure.push_back(*ps);
    }
    ci.systemd_unit = arch::systemd_unit_from_cgroup(path);
    if (!ci.nr_periods) prov.note = "cpu controller not enabled for this cgroup (no bandwidth statistics)";
    return Observation<CgroupInfo>{std::move(ci), std::move(prov)};
}

}  // namespace osie::linux_os

namespace osie {
std::unique_ptr<IOperatingSystem> make_native_os() { return std::make_unique<linux_os::LinuxOperatingSystem>(); }
}  // namespace osie
