// SPDX-License-Identifier: Apache-2.0
// Native macOS (XNU) adapter. Uses only documented public interfaces:
// Mach host APIs, libproc, sysctl(3), getfsstat(2), the PF_ROUTE sysctl and
// NSProcessInfo (via Objective-C++). Never spawns ps/top/vm_stat/etc.
#pragma once

#include <mach/mach.h>

#include <cstdint>
#include <optional>
#include <string>

#include "osie/platform/os.hpp"

namespace osie::macos {

// ---- sysctl helpers (sysctl.cpp) ----------------------------------------
std::optional<std::string> sysctl_string(const char* name);
std::optional<std::uint64_t> sysctl_u64(const char* name);  // accepts 4- or 8-byte integers

/// Converts Mach absolute-time units (used by proc_taskinfo / rusage_info on
/// Apple Silicon: numer/denom = 125/3) to nanoseconds.
class Timebase {
public:
    Timebase();
    [[nodiscard]] std::uint64_t to_ns(std::uint64_t mach_units) const noexcept;

private:
    std::uint32_t numer_{1};
    std::uint32_t denom_{1};
};

// ---- Objective-C++ bridge (native_bridge.mm) ------------------------------
MacPowerState read_power_state();

class MacOperatingSystem final : public IOperatingSystem, public IMacExtensions {
public:
    MacOperatingSystem();
    ~MacOperatingSystem() override;
    MacOperatingSystem(const MacOperatingSystem&) = delete;
    MacOperatingSystem& operator=(const MacOperatingSystem&) = delete;

    [[nodiscard]] Platform platform() const noexcept override { return Platform::macos; }
    [[nodiscard]] ObservationSet supported() const override { return supported_; }

    Result<Observation<SystemInfo>> system_info() override;
    Result<Observation<CpuTopology>> cpu_topology() override;
    Result<Observation<CpuSample>> cpu_sample() override;
    Result<Observation<MemoryMetrics>> memory() override;
    Result<Observation<ProcessSnapshot>> processes(const ProcessQuery& q,
                                                   const CancellationToken& cancel) override;
    Result<Observation<ProcessInfo>> process(Pid pid) override;
    Result<Observation<std::vector<FilesystemInfo>>> filesystems() override;
    Result<Observation<std::vector<NetInterface>>> network_interfaces() override;

    IMacExtensions* macos() noexcept override { return this; }
    Result<Observation<MacPowerState>> power_state() override;

private:
    /// Reads one process. Returns process_exited / permission_denied on failure
    /// of the *basic* info; CPU/memory denial yields a partial ProcessInfo.
    Result<ProcessInfo> read_process(Pid pid, bool include_path) const;

    host_t host_{MACH_PORT_NULL};  // send right owned by this object
    Timebase timebase_;
    ObservationSet supported_;
};

}  // namespace osie::macos
