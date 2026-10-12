// SPDX-License-Identifier: Apache-2.0
// macOS: sysctl helpers, system information and CPU topology.
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <cerrno>
#include <string>

#include "macos_os.hpp"

namespace osie::macos {

std::optional<std::string> sysctl_string(const char* name) {
    std::size_t len = 0;
    if (::sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return std::nullopt;
    std::string s(len, '\0');
    if (::sysctlbyname(name, s.data(), &len, nullptr, 0) != 0) return std::nullopt;
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::optional<std::uint64_t> sysctl_u64(const char* name) {
    std::array<unsigned char, 8> buf{};
    std::size_t len = buf.size();
    if (::sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return std::nullopt;
    if (len == sizeof(std::uint32_t)) {
        std::uint32_t v = 0;
        std::memcpy(&v, buf.data(), sizeof v);
        return v;
    }
    if (len == sizeof(std::uint64_t)) {
        std::uint64_t v = 0;
        std::memcpy(&v, buf.data(), sizeof v);
        return v;
    }
    return std::nullopt;
}

Timebase::Timebase() {
    mach_timebase_info_data_t tb{};
    if (::mach_timebase_info(&tb) == KERN_SUCCESS && tb.denom != 0) {
        numer_ = tb.numer;
        denom_ = tb.denom;
    }
}

std::uint64_t Timebase::to_ns(std::uint64_t units) const noexcept {
    if (numer_ == denom_) return units;
    // 128-bit intermediate avoids overflow for long-running processes.
    return static_cast<std::uint64_t>(static_cast<unsigned __int128>(units) * numer_ / denom_);
}

MacOperatingSystem::MacOperatingSystem() : host_(::mach_host_self()) {
    using K = ObservationKind;
    for (auto k : {K::system_info, K::cpu_topology, K::cpu_times, K::load_average, K::process_list,
                   K::process_detail, K::memory, K::swap, K::filesystems, K::network_interfaces,
                   K::thermal_state}) {
        supported_.add(k);
    }
    // Probe rather than assume: this sysctl is public but not present on all releases.
    if (sysctl_u64("kern.memorystatus_vm_pressure_level")) supported_.add(K::memory_pressure_level);
}

MacOperatingSystem::~MacOperatingSystem() {
    if (host_ != MACH_PORT_NULL) ::mach_port_deallocate(::mach_task_self(), host_);
}

Result<Observation<SystemInfo>> MacOperatingSystem::system_info() {
    auto prov = Provenance::now("sysctl(kern.*,hw.*,machdep.cpu.*)");
    SystemInfo si;
    si.os_name = "macOS";
    si.os_version = sysctl_string("kern.osproductversion").value_or("");
    si.kernel_release = sysctl_string("kern.osrelease").value_or("");
    si.kernel_version = sysctl_string("kern.version").value_or("");
    si.hostname = sysctl_string("kern.hostname").value_or("");
    si.hardware_model = sysctl_string("hw.model").value_or("");
    si.cpu_brand = sysctl_string("machdep.cpu.brand_string").value_or("");
    si.mem_total_bytes = sysctl_u64("hw.memsize").value_or(0);
    si.page_size = static_cast<std::uint64_t>(::getpagesize());
    struct utsname u {};
    if (::uname(&u) == 0) si.arch = u.machine;

    struct timeval boot {};
    std::size_t len = sizeof boot;
    if (::sysctlbyname("kern.boottime", &boot, &len, nullptr, 0) == 0) si.boot_time_unix = boot.tv_sec;

    if (si.os_version.empty() || si.mem_total_bytes == 0) {
        prov.validity = Validity::partial;
        prov.note = "some sysctl keys were unavailable";
    }
    return Observation<SystemInfo>{std::move(si), std::move(prov)};
}

Result<Observation<CpuTopology>> MacOperatingSystem::cpu_topology() {
    auto prov = Provenance::now("sysctl(hw.physicalcpu,hw.logicalcpu,hw.perflevel*)");
    CpuTopology t;
    t.packages = static_cast<std::uint32_t>(sysctl_u64("hw.packages").value_or(1));
    t.physical_cores = static_cast<std::uint32_t>(sysctl_u64("hw.physicalcpu").value_or(0));
    t.logical_cpus = static_cast<std::uint32_t>(sysctl_u64("hw.logicalcpu").value_or(0));
    t.online_cpus = static_cast<std::uint32_t>(sysctl_u64("hw.activecpu").value_or(t.logical_cpus));
    if (t.logical_cpus == 0) {
        return make_error(Errc::unavailable, "hw.logicalcpu not readable");
    }
    if (auto v = sysctl_u64("hw.l1dcachesize")) t.l1d_cache_bytes = *v;
    if (auto v = sysctl_u64("hw.l2cachesize")) t.l2_cache_bytes = *v;
    if (auto v = sysctl_u64("hw.l3cachesize")) t.l3_cache_bytes = *v;
    if (auto v = sysctl_u64("hw.cpufrequency")) t.nominal_frequency_hz = *v;  // absent on Apple Silicon

    // Apple Silicon / heterogeneous topologies: hw.nperflevels + hw.perflevelN.*
    const auto levels = sysctl_u64("hw.nperflevels").value_or(0);
    for (std::uint64_t i = 0; i < levels; ++i) {
        const std::string base = "hw.perflevel" + std::to_string(i) + ".";
        CpuCluster c;
        c.name = sysctl_string((base + "name").c_str()).value_or("perflevel" + std::to_string(i));
        c.physical_cores = static_cast<std::uint32_t>(sysctl_u64((base + "physicalcpu").c_str()).value_or(0));
        c.logical_cpus = static_cast<std::uint32_t>(sysctl_u64((base + "logicalcpu").c_str()).value_or(0));
        if (auto v = sysctl_u64((base + "l2cachesize").c_str())) c.l2_cache_bytes = *v;
        t.clusters.push_back(std::move(c));
    }
    if (t.clusters.empty()) {
        t.clusters.push_back(CpuCluster{"cpu-package-0", t.physical_cores, t.logical_cpus, t.l2_cache_bytes});
    }
    return Observation<CpuTopology>{std::move(t), std::move(prov)};
}

Result<Observation<MacPowerState>> MacOperatingSystem::power_state() {
    return Observation<MacPowerState>{read_power_state(), Provenance::now("NSProcessInfo.thermalState")};
}

}  // namespace osie::macos
