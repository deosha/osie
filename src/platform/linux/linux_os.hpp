// SPDX-License-Identifier: Apache-2.0
// Native Linux adapter: procfs, sysfs, cgroup v2, PSI and rtnetlink.
// Never spawns ps/top/free/etc.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/platform/os.hpp"

namespace osie::linux_os {

// ---- procfs/sysfs helpers (procfs.cpp) -----------------------------------
/// Reads a whole (pseudo-)file. procfs reports size 0, so read until EOF.
Result<std::string> read_file(const std::string& path, std::size_t limit = 1 << 20);
std::optional<std::uint64_t> read_u64(const std::string& path);
/// "key: value kB" style files (/proc/meminfo, /proc/<pid>/status) -> value (kB scaled to bytes if `kb`).
std::optional<std::uint64_t> kv_field(std::string_view text, std::string_view key, bool kb_to_bytes);
/// "key value" files (/proc/vmstat, cpu.stat).
std::optional<std::uint64_t> space_field(std::string_view text, std::string_view key);
/// Parses CPU list syntax "0-3,8,10-11".
std::vector<unsigned> parse_cpu_list(std::string_view s);
/// Parses a PSI file body into a PressureStall.
std::optional<PressureStall> parse_psi(std::string_view body, PsiResource r);

struct ProcStat {
    char state{'?'};
    Pid ppid{0};
    std::uint64_t utime_ticks{0}, stime_ticks{0}, majflt{0}, start_ticks{0}, vsize{0}, rss_pages{0};
    std::int32_t nice{0};
    std::uint32_t threads{0};
    std::string comm;
};
/// Parses /proc/<pid>/stat. Handles ')' and spaces inside comm.
std::optional<ProcStat> parse_proc_stat(std::string_view s);

class LinuxOperatingSystem final : public IOperatingSystem, public ILinuxExtensions {
public:
    /// `root` allows tests to point the adapter at a fixture tree instead of "/".
    explicit LinuxOperatingSystem(std::string root = "");

    [[nodiscard]] Platform platform() const noexcept override { return Platform::linux_os; }
    [[nodiscard]] ObservationSet supported() const override { return supported_; }

    Result<Observation<SystemInfo>> system_info() override;
    Result<Observation<CpuTopology>> cpu_topology() override;
    Result<Observation<CpuSample>> cpu_sample() override;
    Result<Observation<MemoryMetrics>> memory() override;
    Result<Observation<ProcessSnapshot>> processes(const ProcessQuery& q, const CancellationToken& cancel) override;
    Result<Observation<ProcessInfo>> process(Pid pid) override;
    Result<Observation<std::vector<FilesystemInfo>>> filesystems() override;
    Result<Observation<std::vector<NetInterface>>> network_interfaces() override;

    ILinuxExtensions* linux_ext() noexcept override { return this; }
    Result<Observation<std::vector<PressureStall>>> pressure() override;
    Result<Observation<CgroupInfo>> cgroup(const std::string& path) override;
    Result<std::string> cgroup_of(Pid pid) override;

private:
    Result<ProcessInfo> read_process(Pid pid, bool include_path) const;
    Result<std::vector<NetInterface>> netlink_links() const;
    Result<std::vector<NetInterface>> procfs_links() const;
    [[nodiscard]] std::string p(std::string_view rel) const { return root_ + std::string(rel); }

    std::string root_;
    std::string cgroup_mount_;  // e.g. "/sys/fs/cgroup"; empty if no cgroup2
    long clk_tck_{100};
    std::uint64_t page_size_{4096};
    std::uint64_t boot_time_s_{0};
    ObservationSet supported_;
};

}  // namespace osie::linux_os
