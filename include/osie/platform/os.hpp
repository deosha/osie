// SPDX-License-Identifier: Apache-2.0
// Platform interface contracts.
//
// IOperatingSystem covers only concepts that genuinely exist on both XNU and
// Linux. Platform-specific facilities are exposed through typed extension
// interfaces (IMacExtensions / ILinuxExtensions) rather than being forced into
// a fake universal API. Callers must check for nullptr.
#pragma once

#include <memory>
#include <vector>

#include "osie/core/observation.hpp"
#include "osie/core/result.hpp"
#include "osie/platform/types.hpp"

namespace osie {

/// Kinds of observation a platform may provide. The capability registry maps
/// each observe.* capability to one of these.
enum class ObservationKind : std::uint8_t {
    system_info,
    cpu_topology,
    cpu_times,
    load_average,
    process_list,
    process_detail,
    memory,
    memory_pressure_level,  // macOS
    swap,
    filesystems,
    network_interfaces,
    thermal_state,          // macOS
    pressure_stall,         // Linux PSI
    cgroup,                 // Linux cgroup v2
    count_
};

std::string_view to_string(ObservationKind k) noexcept;

/// Set of supported ObservationKinds, reported by each adapter at runtime
/// after probing (e.g. PSI may be compiled-in but disabled by the kernel).
class ObservationSet {
public:
    void add(ObservationKind k) noexcept { bits_ |= bit(k); }
    [[nodiscard]] bool contains(ObservationKind k) const noexcept { return (bits_ & bit(k)) != 0; }
    [[nodiscard]] std::vector<ObservationKind> list() const;

private:
    static constexpr std::uint32_t bit(ObservationKind k) noexcept {
        return 1U << static_cast<unsigned>(k);
    }
    std::uint32_t bits_{0};
};

class IMacExtensions {
public:
    virtual ~IMacExtensions() = default;
    virtual Result<Observation<MacPowerState>> power_state() = 0;
};

class ILinuxExtensions {
public:
    virtual ~ILinuxExtensions() = default;
    /// System-wide PSI from /proc/pressure/*.
    virtual Result<Observation<std::vector<PressureStall>>> pressure() = 0;
    /// cgroup v2 state for a path relative to the cgroup2 mount.
    virtual Result<Observation<CgroupInfo>> cgroup(const std::string& path) = 0;
    /// cgroup of the given process (reads /proc/<pid>/cgroup).
    virtual Result<std::string> cgroup_of(Pid pid) = 0;
};

class IOperatingSystem {
public:
    virtual ~IOperatingSystem() = default;

    [[nodiscard]] virtual Platform platform() const noexcept = 0;
    /// Observation kinds this adapter can serve on the running kernel.
    [[nodiscard]] virtual ObservationSet supported() const = 0;

    virtual Result<Observation<SystemInfo>> system_info() = 0;
    virtual Result<Observation<CpuTopology>> cpu_topology() = 0;
    virtual Result<Observation<CpuSample>> cpu_sample() = 0;
    virtual Result<Observation<MemoryMetrics>> memory() = 0;
    virtual Result<Observation<ProcessSnapshot>> processes(const ProcessQuery& q,
                                                           const CancellationToken& cancel) = 0;
    virtual Result<Observation<ProcessInfo>> process(Pid pid) = 0;
    virtual Result<Observation<std::vector<FilesystemInfo>>> filesystems() = 0;
    virtual Result<Observation<std::vector<NetInterface>>> network_interfaces() = 0;

    virtual IMacExtensions* macos() noexcept { return nullptr; }
    virtual ILinuxExtensions* linux_ext() noexcept { return nullptr; }
};

/// Returns the native adapter for the platform this binary was built for.
std::unique_ptr<IOperatingSystem> make_native_os();

}  // namespace osie
