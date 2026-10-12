// SPDX-License-Identifier: Apache-2.0
// Plain data types returned by platform adapters.
//
// Design rule: adapters return *raw cumulative counters* exactly as the kernel
// exposes them. Rates and percentages are derived in the state layer from two
// timestamped samples. Fields that a platform cannot provide are std::nullopt,
// never zero.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "osie/core/observation.hpp"

namespace osie {

using Pid = std::int32_t;
using Uid = std::uint32_t;

struct SystemInfo {
    std::string os_name;         // "macOS", "Linux"
    std::string os_version;      // "15.7.3", distro VERSION_ID
    std::string kernel_release;  // "24.6.0" / uname -r equivalent
    std::string kernel_version;  // full kernel version string
    std::string hostname;
    std::string hardware_model;  // "Mac16,7" / DMI product name if readable
    std::string cpu_brand;       // "Apple M4 Pro"
    std::string arch;            // "arm64", "x86_64"
    std::uint64_t mem_total_bytes{0};
    std::uint64_t page_size{0};
    std::optional<std::int64_t> boot_time_unix;
    bool in_container{false};    // Linux: cgroup/namespace heuristics
};

/// Heterogeneous CPU clusters (Apple P/E cores, Intel hybrid, big.LITTLE).
struct CpuCluster {
    std::string name;  // "Performance", "Efficiency", "cpu-package-0"
    std::uint32_t physical_cores{0};
    std::uint32_t logical_cpus{0};
    std::optional<std::uint64_t> l2_cache_bytes;
};

struct CpuTopology {
    std::uint32_t packages{1};
    std::uint32_t physical_cores{0};
    std::uint32_t logical_cpus{0};
    std::uint32_t online_cpus{0};
    std::vector<CpuCluster> clusters;
    std::optional<std::uint64_t> l1d_cache_bytes;
    std::optional<std::uint64_t> l2_cache_bytes;
    std::optional<std::uint64_t> l3_cache_bytes;
    std::optional<std::uint64_t> nominal_frequency_hz;  // often unavailable on Apple Silicon
};

/// Cumulative CPU time, in kernel ticks (USER_HZ on Linux, host ticks on XNU).
/// Fields not tracked by the kernel stay 0 and are flagged in CpuSample.
struct CpuTicks {
    std::uint64_t user{0}, nice{0}, system{0}, idle{0};
    std::uint64_t iowait{0}, irq{0}, softirq{0}, steal{0};
    [[nodiscard]] std::uint64_t total() const noexcept {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    [[nodiscard]] std::uint64_t busy() const noexcept { return total() - idle - iowait; }
};

struct LoadAverage {
    double one{0}, five{0}, fifteen{0};
};

struct CpuSample {
    CpuTicks aggregate;
    std::vector<CpuTicks> per_cpu;
    bool has_iowait{false};  // Linux only
    bool has_steal{false};   // Linux only (virtualised hosts)
    std::optional<LoadAverage> load;
    std::optional<std::uint32_t> runnable_tasks;          // Linux /proc/stat procs_running
    std::optional<std::uint32_t> blocked_tasks;           // Linux procs_blocked
    std::optional<std::uint64_t> context_switches_total;  // cumulative
};

enum class MemoryPressureLevel : std::uint8_t { normal, warning, critical };

constexpr std::string_view to_string(MemoryPressureLevel l) noexcept {
    switch (l) {
        case MemoryPressureLevel::normal: return "normal";
        case MemoryPressureLevel::warning: return "warning";
        case MemoryPressureLevel::critical: return "critical";
    }
    return "?";
}

struct MemoryMetrics {
    std::uint64_t total_bytes{0};
    std::uint64_t page_size{0};
    std::optional<std::uint64_t> free_bytes;
    std::optional<std::uint64_t> available_bytes;  // Linux MemAvailable; macOS estimate (free+inactive+purgeable)
    std::optional<std::uint64_t> active_bytes;
    std::optional<std::uint64_t> inactive_bytes;
    std::optional<std::uint64_t> wired_bytes;       // macOS wired / Linux unevictable-ish (not mapped)
    std::optional<std::uint64_t> compressed_bytes;  // macOS: pages held by the compressor
    std::optional<std::uint64_t> file_backed_bytes; // page cache
    std::optional<std::uint64_t> swap_total_bytes;
    std::optional<std::uint64_t> swap_used_bytes;
    // cumulative event counters (pages or events since boot)
    std::optional<std::uint64_t> pageins;
    std::optional<std::uint64_t> pageouts;
    std::optional<std::uint64_t> swapins;
    std::optional<std::uint64_t> swapouts;
    std::optional<std::uint64_t> compressions;
    std::optional<std::uint64_t> decompressions;
    std::optional<std::uint64_t> faults;
    std::optional<std::uint64_t> major_faults;
    /// macOS kernel memory-pressure level (kern.memorystatus_vm_pressure_level).
    /// Linux has no equivalent level; it exposes PSI instead (see LinuxExtensions).
    std::optional<MemoryPressureLevel> pressure_level;
};

enum class ProcessRunState : std::uint8_t { unknown, running, sleeping, stopped, zombie, idle, disk_wait };

constexpr std::string_view to_string(ProcessRunState s) noexcept {
    switch (s) {
        case ProcessRunState::unknown: return "unknown";
        case ProcessRunState::running: return "running";
        case ProcessRunState::sleeping: return "sleeping";
        case ProcessRunState::stopped: return "stopped";
        case ProcessRunState::zombie: return "zombie";
        case ProcessRunState::idle: return "idle";
        case ProcessRunState::disk_wait: return "disk_wait";
    }
    return "?";
}

struct ProcessInfo {
    Pid pid{0};
    Pid ppid{0};
    Uid uid{0};
    std::string name;  // short command name
    std::string path;  // executable path when readable
    ProcessRunState state{ProcessRunState::unknown};
    /// Start time disambiguates PID reuse: (pid, start_time) is the identity.
    std::optional<std::uint64_t> start_time_us;
    std::optional<std::uint64_t> cpu_user_ns;   // cumulative
    std::optional<std::uint64_t> cpu_system_ns; // cumulative
    std::optional<std::uint64_t> resident_bytes;
    std::optional<std::uint64_t> footprint_bytes;  // macOS phys_footprint (Activity Monitor "Memory")
    std::optional<std::uint64_t> virtual_bytes;
    std::optional<std::uint32_t> threads;
    std::optional<std::uint64_t> major_faults;  // cumulative
    std::optional<std::int32_t> nice;
    std::optional<std::string> cgroup;  // Linux: unified (v2) cgroup path
    Validity validity{Validity::valid};  // partial when CPU/memory fields were denied

    [[nodiscard]] std::optional<std::uint64_t> cpu_total_ns() const {
        if (!cpu_user_ns || !cpu_system_ns) return std::nullopt;
        return *cpu_user_ns + *cpu_system_ns;
    }
    /// Best available "how much memory does this use" figure.
    [[nodiscard]] std::optional<std::uint64_t> memory_bytes() const {
        return footprint_bytes ? footprint_bytes : resident_bytes;
    }
};

struct ProcessSnapshot {
    std::vector<ProcessInfo> processes;
    std::uint32_t partial_count{0};  // processes whose CPU/memory fields were denied
    std::uint32_t vanished_count{0}; // exited between enumeration and inspection
};

struct ProcessQuery {
    bool include_kernel_threads{false};  // Linux kthreads
    bool include_paths{true};
};

struct FilesystemInfo {
    std::string mount_point;
    std::string device;
    std::string fs_type;
    std::uint64_t total_bytes{0};
    std::uint64_t free_bytes{0};
    std::uint64_t available_bytes{0};  // free to unprivileged users
    std::optional<std::uint64_t> files_total;
    std::optional<std::uint64_t> files_free;
    bool read_only{false};
    bool local{true};
};

struct NetInterface {
    std::string name;
    bool up{false};
    bool loopback{false};
    std::optional<std::uint32_t> mtu;
    std::optional<std::uint64_t> rx_bytes, tx_bytes;      // cumulative
    std::optional<std::uint64_t> rx_packets, tx_packets;  // cumulative
    std::optional<std::uint64_t> rx_errors, tx_errors;    // cumulative
    std::optional<std::uint64_t> rx_drops;                // cumulative
    std::vector<std::string> addresses;
};

// ---- macOS-only ---------------------------------------------------------
/// NSProcessInfo.thermalState. A qualitative OS signal, not a temperature.
enum class ThermalState : std::uint8_t { nominal, fair, serious, critical };

constexpr std::string_view to_string(ThermalState t) noexcept {
    switch (t) {
        case ThermalState::nominal: return "nominal";
        case ThermalState::fair: return "fair";
        case ThermalState::serious: return "serious";
        case ThermalState::critical: return "critical";
    }
    return "?";
}

struct MacPowerState {
    ThermalState thermal{ThermalState::nominal};
    bool low_power_mode{false};
};

// ---- Linux-only ----------------------------------------------------------
/// One PSI line, see Documentation/accounting/psi.rst.
struct PsiLine {
    double avg10{0}, avg60{0}, avg300{0};
    std::uint64_t total_us{0};  // cumulative stall time
};

enum class PsiResource : std::uint8_t { cpu, memory, io };

constexpr std::string_view to_string(PsiResource r) noexcept {
    switch (r) {
        case PsiResource::cpu: return "cpu";
        case PsiResource::memory: return "memory";
        case PsiResource::io: return "io";
    }
    return "?";
}

struct PressureStall {
    PsiResource resource{PsiResource::cpu};
    PsiLine some;
    std::optional<PsiLine> full;  // CPU "full" exists only on newer kernels
};

/// cgroup v2 controller state, see Documentation/admin-guide/cgroup-v2.rst.
struct CgroupInfo {
    std::string path;  // relative to the cgroup2 mount, e.g. "/system.slice/nginx.service"
    std::optional<std::int64_t> cpu_quota_us;   // cpu.max quota; nullopt = "max" (unlimited)
    std::optional<std::uint64_t> cpu_period_us;
    std::optional<std::uint64_t> cpu_weight;
    std::optional<std::uint64_t> usage_usec, user_usec, system_usec;   // cpu.stat, cumulative
    std::optional<std::uint64_t> nr_periods, nr_throttled, throttled_usec;
    std::optional<std::uint64_t> memory_current, memory_max, memory_high;
    std::optional<std::uint64_t> pids_current;
    std::optional<std::uint32_t> cpuset_cpus;  // CPUs in cpuset.cpus.effective
    std::vector<PressureStall> pressure;  // per-cgroup PSI files
    std::optional<std::string> systemd_unit;  // derived from path: "*.service" / "*.scope"
};

}  // namespace osie
