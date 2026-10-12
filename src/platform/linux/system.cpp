// SPDX-License-Identifier: Apache-2.0
// Linux: system information and CPU topology from procfs/sysfs.
#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <set>

#include "linux_os.hpp"

namespace osie::linux_os {

namespace {
std::string os_release_value(std::string_view body, std::string_view key) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto eol = body.find('\n', pos);
        auto line = body.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? body.size() : eol + 1;
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
            auto v = line.substr(key.size() + 1);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
            return std::string(v);
        }
    }
    return {};
}

std::string trim_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

LinuxOperatingSystem::LinuxOperatingSystem(std::string root) : root_(std::move(root)) {
    clk_tck_ = ::sysconf(_SC_CLK_TCK);
    if (clk_tck_ <= 0) clk_tck_ = 100;
    const long ps = ::sysconf(_SC_PAGESIZE);
    page_size_ = ps > 0 ? static_cast<std::uint64_t>(ps) : 4096;
    if (auto st = read_file(p("/proc/stat"))) boot_time_s_ = space_field(*st, "btime").value_or(0);

    using K = ObservationKind;
    for (auto k : {K::system_info, K::cpu_topology, K::cpu_times, K::load_average, K::process_list,
                   K::process_detail, K::memory, K::swap, K::filesystems, K::network_interfaces}) {
        supported_.add(k);
    }
    // PSI: absent without CONFIG_PSI; reads fail with EOPNOTSUPP when booted with psi=0.
    if (auto r = read_file(p("/proc/pressure/cpu")); r && parse_psi(*r, PsiResource::cpu)) supported_.add(K::pressure_stall);
    // cgroup v2: locate the unified hierarchy mount.
    if (auto mi = read_file(p("/proc/self/mountinfo"))) {
        std::string_view body = *mi;
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto eol = body.find('\n', pos);
            auto line = body.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
            pos = eol == std::string_view::npos ? body.size() : eol + 1;
            const auto sep = line.find(" - ");
            if (sep == std::string_view::npos) continue;
            if (line.substr(sep + 3).rfind("cgroup2 ", 0) != 0) continue;
            // fields: id parent major:minor root mount_point ...
            std::size_t f = 0, start = 0;
            for (std::size_t i = 0; i <= sep && f < 5; ++i) {
                if (i == sep || line[i] == ' ') {
                    if (f == 4) cgroup_mount_ = std::string(line.substr(start, i - start));
                    ++f;
                    start = i + 1;
                }
            }
            break;
        }
    }
    if (!cgroup_mount_.empty() && read_file(root_ + cgroup_mount_ + "/cgroup.controllers")) supported_.add(K::cgroup);
}

Result<Observation<SystemInfo>> LinuxOperatingSystem::system_info() {
    auto prov = Provenance::now("uname(2)+procfs:/proc/meminfo+/etc/os-release");
    SystemInfo si;
    si.os_name = "Linux";
    struct utsname u {};
    if (::uname(&u) == 0) {
        si.kernel_release = u.release;
        si.kernel_version = u.version;
        si.hostname = u.nodename;
        si.arch = u.machine;
    }
    if (auto osr = read_file(p("/etc/os-release"))) {
        const auto pretty = os_release_value(*osr, "PRETTY_NAME");
        if (!pretty.empty()) si.os_name = "Linux (" + pretty + ")";
        si.os_version = os_release_value(*osr, "VERSION_ID");
    }
    if (auto prod = read_file(p("/sys/class/dmi/id/product_name"))) si.hardware_model = trim_nl(*prod);
    if (auto ci = read_file(p("/proc/cpuinfo"))) {
        // x86 exposes "model name"; arm64 generally does not.
        const auto pos = ci->find("model name");
        if (pos != std::string::npos) {
            const auto colon = ci->find(':', pos);
            const auto eol = ci->find('\n', pos);
            if (colon != std::string::npos && colon < eol) si.cpu_brand = trim_nl(ci->substr(colon + 2, eol - colon - 2));
        }
    }
    if (auto mi = read_file(p("/proc/meminfo"))) si.mem_total_bytes = kv_field(*mi, "MemTotal", true).value_or(0);
    si.page_size = page_size_;
    if (boot_time_s_ > 0) si.boot_time_unix = static_cast<std::int64_t>(boot_time_s_);
    // Container heuristics (documented as such): runtime marker files or a
    // non-root cgroup namespace view.
    si.in_container = ::access(p("/.dockerenv").c_str(), F_OK) == 0 ||
                      ::access(p("/run/.containerenv").c_str(), F_OK) == 0;
    if (si.mem_total_bytes == 0) prov.validity = Validity::partial;
    return Observation<SystemInfo>{std::move(si), std::move(prov)};
}

Result<Observation<CpuTopology>> LinuxOperatingSystem::cpu_topology() {
    auto prov = Provenance::now("sysfs:/sys/devices/system/cpu");
    CpuTopology t;
    const long conf = ::sysconf(_SC_NPROCESSORS_CONF);
    t.logical_cpus = conf > 0 ? static_cast<std::uint32_t>(conf) : 0;
    if (auto on = read_file(p("/sys/devices/system/cpu/online")))
        t.online_cpus = static_cast<std::uint32_t>(parse_cpu_list(*on).size());
    else
        t.online_cpus = t.logical_cpus;
    std::set<std::pair<std::uint64_t, std::uint64_t>> cores;
    std::set<std::uint64_t> packages;
    for (std::uint32_t c = 0; c < t.logical_cpus; ++c) {
        const std::string base = p("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/");
        auto pkg = read_u64(base + "physical_package_id");
        auto core = read_u64(base + "core_id");
        if (!pkg || !core) continue;
        packages.insert(*pkg);
        cores.insert({*pkg, *core});
    }
    t.physical_cores = cores.empty() ? t.logical_cpus : static_cast<std::uint32_t>(cores.size());
    t.packages = packages.empty() ? 1 : static_cast<std::uint32_t>(packages.size());
    for (int idx = 0; idx < 8; ++idx) {
        const std::string base = p("/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(idx) + "/");
        auto level = read_u64(base + "level");
        auto type = read_file(base + "type");
        auto size = read_file(base + "size");
        if (!level || !type || !size) continue;
        std::uint64_t bytes = std::strtoull(size->c_str(), nullptr, 10);
        if (size->find('K') != std::string::npos) bytes *= 1024;
        if (size->find('M') != std::string::npos) bytes *= 1024ULL * 1024ULL;
        if (*level == 1 && type->rfind("Data", 0) == 0) t.l1d_cache_bytes = bytes;
        if (*level == 2) t.l2_cache_bytes = bytes;
        if (*level == 3) t.l3_cache_bytes = bytes;
    }
    // cpuset restriction (containers / taskset) is not the same as topology:
    cpu_set_t set;
    CPU_ZERO(&set);
    std::uint32_t allowed = 0;
    if (::sched_getaffinity(0, sizeof set, &set) == 0) allowed = static_cast<std::uint32_t>(CPU_COUNT(&set));
    for (std::uint32_t pk = 0; pk < t.packages; ++pk) {
        t.clusters.push_back(CpuCluster{"cpu-package-" + std::to_string(pk), t.physical_cores / t.packages,
                                        t.logical_cpus / t.packages, t.l2_cache_bytes});
    }
    if (allowed > 0 && allowed < t.online_cpus) {
        prov.note = "this process may only run on " + std::to_string(allowed) + " of " +
                    std::to_string(t.online_cpus) + " online CPUs (cpuset/affinity)";
    }
    if (auto f = read_u64(p("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"))) t.nominal_frequency_hz = *f * 1000;
    return Observation<CpuTopology>{std::move(t), std::move(prov)};
}

}  // namespace osie::linux_os
