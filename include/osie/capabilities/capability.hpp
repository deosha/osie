// SPDX-License-Identifier: Apache-2.0
// Typed capability registry, safety policy and audit log.
//
// Every operation OSIE can perform is a registered capability. Observation
// capabilities are executed only through ObservationBroker, which asks the
// SafetyPolicy first and records the decision in the AuditLog. Action
// capabilities (process termination, renice, sysctl writes, ...) are *declared*
// so that requests for them can be recognised, explained and refused, but V1
// contains no executor for them at all.
#pragma once

#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/core/observation.hpp"
#include "osie/core/result.hpp"
#include "osie/platform/os.hpp"

namespace osie {

enum class RiskLevel : std::uint8_t { none, low, medium, high, critical };
std::string_view to_string(RiskLevel r) noexcept;

enum class CapabilityKind : std::uint8_t { observe, act };

/// Privileges a capability may require. Bit flags.
enum class Permission : std::uint16_t {
    none = 0,
    read_own_processes = 1U << 0U,    // unprivileged
    read_other_processes = 1U << 1U,  // root or task_for_pid entitlement / CAP_SYS_PTRACE
    root = 1U << 2U,
    cgroup_write = 1U << 3U,
    signal_process = 1U << 4U,
    kubernetes_rbac = 1U << 5U,
};

class PermissionSet {
public:
    constexpr PermissionSet() = default;
    constexpr PermissionSet(std::initializer_list<Permission> ps) {
        for (auto p : ps) bits_ |= static_cast<std::uint16_t>(p);
    }
    [[nodiscard]] constexpr bool contains(Permission p) const noexcept {
        return (bits_ & static_cast<std::uint16_t>(p)) != 0;
    }
    [[nodiscard]] constexpr bool privileged() const noexcept {
        return contains(Permission::root) || contains(Permission::read_other_processes) ||
               contains(Permission::cgroup_write) || contains(Permission::signal_process) ||
               contains(Permission::kubernetes_rbac);
    }
    [[nodiscard]] std::string to_string() const;

private:
    std::uint16_t bits_{0};
};

struct CapabilityDescriptor {
    std::string id;  // dotted, lowercase: "observe.cpu.times", "act.process.terminate"
    std::string description;
    CapabilityKind kind{CapabilityKind::observe};
    RiskLevel risk{RiskLevel::none};
    PlatformSet platforms;
    PermissionSet required_permissions;
    bool requires_confirmation{false};
    bool reversible{true};
    bool has_side_effects{false};
    std::vector<ObservationKind> observations;  // observe.* only
    std::string preconditions;
    std::string side_effects;
    std::string postconditions;
    std::string rollback;
    bool implemented{false};  // an executor exists in this build
};

/// Validates descriptor consistency; returns the list of violations (empty = valid).
std::vector<std::string> validate(const CapabilityDescriptor& d);

class CapabilityRegistry {
public:
    /// Rejects invalid or duplicate descriptors.
    Result<void> add(CapabilityDescriptor d);
    [[nodiscard]] const CapabilityDescriptor* find(std::string_view id) const;
    [[nodiscard]] const CapabilityDescriptor* for_observation(ObservationKind k) const;
    [[nodiscard]] const std::vector<CapabilityDescriptor>& all() const noexcept { return caps_; }

    /// The built-in V1 catalogue.
    static CapabilityRegistry builtin();

private:
    std::vector<CapabilityDescriptor> caps_;
};

struct PolicyDecision {
    std::string capability;
    bool allowed{false};
    std::string reason;
    WallTime at{};
};

class AuditLog {
public:
    explicit AuditLog(std::size_t capacity = 1024) : capacity_(capacity) {}
    /// Optional JSON-lines sink. Failure to open is reported, not fatal.
    Result<void> open_file(const std::string& path);
    void record(const PolicyDecision& d);
    [[nodiscard]] std::vector<PolicyDecision> recent(std::size_t n) const;
    [[nodiscard]] std::size_t denied_count() const;

private:
    mutable std::mutex mu_;
    std::deque<PolicyDecision> entries_;
    std::size_t capacity_;
    std::size_t denied_{0};
    std::ofstream file_;
};

struct PolicyConfig {
    bool read_only{true};                 // V1: hard-wired; actions are never allowed
    bool allow_privileged_reads{false};   // never escalate; only use what the user already has
};

class SafetyPolicy {
public:
    explicit SafetyPolicy(PolicyConfig cfg = {}) : cfg_(cfg) {}
    [[nodiscard]] PolicyDecision authorize(const CapabilityDescriptor& d, Platform platform,
                                           const ObservationSet& supported) const;
    [[nodiscard]] const PolicyConfig& config() const noexcept { return cfg_; }

private:
    PolicyConfig cfg_;
};

/// The only path from the engine to the OS adapter. Each call is authorised and audited.
class ObservationBroker {
public:
    ObservationBroker(IOperatingSystem& os, const CapabilityRegistry& reg, const SafetyPolicy& pol,
                      AuditLog& audit)
        : os_(os), reg_(reg), pol_(pol), audit_(audit) {}

    Result<void> authorize(ObservationKind k);

    Result<Observation<SystemInfo>> system_info();
    Result<Observation<CpuTopology>> cpu_topology();
    Result<Observation<CpuSample>> cpu_sample();
    Result<Observation<MemoryMetrics>> memory();
    Result<Observation<ProcessSnapshot>> processes(const ProcessQuery& q, const CancellationToken& c);
    Result<Observation<ProcessInfo>> process(Pid pid);
    Result<Observation<std::vector<FilesystemInfo>>> filesystems();
    Result<Observation<std::vector<NetInterface>>> network_interfaces();
    Result<Observation<MacPowerState>> power_state();
    Result<Observation<std::vector<PressureStall>>> pressure();
    Result<Observation<CgroupInfo>> cgroup(const std::string& path);
    Result<std::string> cgroup_of(Pid pid);

    [[nodiscard]] Platform platform() const noexcept { return os_.platform(); }
    [[nodiscard]] ObservationSet supported() const { return os_.supported(); }

private:
    IOperatingSystem& os_;
    const CapabilityRegistry& reg_;
    const SafetyPolicy& pol_;
    AuditLog& audit_;
};

}  // namespace osie
