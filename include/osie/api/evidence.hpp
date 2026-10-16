// SPDX-License-Identifier: Apache-2.0
// Evidence for API answers: the agent's published snapshots and history,
// never a new sample.
//
// RefusingOs is the operating-system backend of the API's Engine. It reports
// the agent's platform and supported observation kinds, and refuses every
// observation call. Any code path in the engine that would observe on demand
// (a sampling window, a live capacity read) therefore fails with an explicit
// reason instead of starting a second observer.
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "osie/agent/runtime.hpp"
#include "osie/api/peer.hpp"
#include "osie/engine/engine.hpp"

namespace osie::api {

inline constexpr const char* kNoOnDemand =
    "not available through the agent API: answers use the agent's published snapshot, which does not include this";

class RefusingOs final : public IOperatingSystem, public ILinuxExtensions, public IMacExtensions {
public:
    RefusingOs(Platform p, ObservationSet supported) : platform_(p), supported_(supported) {}
    [[nodiscard]] Platform platform() const noexcept override { return platform_; }
    [[nodiscard]] ObservationSet supported() const override { return supported_; }
    /// Observation calls refused so far, on all threads (tests assert this
    /// stays 0 for answers the snapshot covers).
    [[nodiscard]] std::uint64_t refused_calls() const noexcept { return calls_.load(); }
    /// Refused calls made by the calling thread (per-request accounting: an
    /// answer runs on one thread).
    [[nodiscard]] static std::uint64_t refused_on_this_thread() noexcept { return thread_calls_; }

    Result<Observation<SystemInfo>> system_info() override { return refuse<SystemInfo>(); }
    Result<Observation<CpuTopology>> cpu_topology() override { return refuse<CpuTopology>(); }
    Result<Observation<CpuSample>> cpu_sample() override { return refuse<CpuSample>(); }
    Result<Observation<MemoryMetrics>> memory() override { return refuse<MemoryMetrics>(); }
    Result<Observation<ProcessSnapshot>> processes(const ProcessQuery&, const CancellationToken&) override {
        return refuse<ProcessSnapshot>();
    }
    Result<Observation<ProcessInfo>> process(Pid) override { return refuse<ProcessInfo>(); }
    Result<Observation<std::vector<FilesystemInfo>>> filesystems() override { return refuse<std::vector<FilesystemInfo>>(); }
    Result<Observation<std::vector<NetInterface>>> network_interfaces() override {
        return refuse<std::vector<NetInterface>>();
    }
    Result<Observation<MacPowerState>> power_state() override { return refuse<MacPowerState>(); }
    Result<Observation<std::vector<PressureStall>>> pressure() override { return refuse<std::vector<PressureStall>>(); }
    Result<Observation<CgroupInfo>> cgroup(const std::string&) override { return refuse<CgroupInfo>(); }
    Result<std::string> cgroup_of(Pid) override {
        ++calls_;
        ++thread_calls_;
        return make_error(Errc::unavailable, kNoOnDemand);
    }
    Result<Observation<CpuCapacity>> cpu_capacity(Pid) override { return refuse<CpuCapacity>(); }
    IMacExtensions* macos() noexcept override { return platform_ == Platform::macos ? this : nullptr; }
    ILinuxExtensions* linux_ext() noexcept override { return platform_ == Platform::linux_os ? this : nullptr; }

private:
    template <class T>
    Result<Observation<T>> refuse() {
        ++calls_;
        ++thread_calls_;
        return make_error(Errc::unavailable, kNoOnDemand);
    }
    Platform platform_;
    ObservationSet supported_;
    std::atomic<std::uint64_t> calls_{0};
    static thread_local std::uint64_t thread_calls_;
};

/// What the evidence for one answer was, for the response's status and
/// limitations.
struct EvidenceReport {
    bool consulted{false};    // the answer needed observations
    bool no_snapshot{false};  // the agent has not published one yet
    bool stale{false};        // the newest one is older than allowed
    std::optional<double> age_ms;
    std::uint64_t seq{0};
    std::optional<WallTime> collected_at;
    std::vector<ObservationKind> missing;  // needed but not in the snapshot
    std::string cgroup_note;               // how cgroup rates were obtained, or why not
};

/// One request's evidence: the agent's newest snapshot (if fresh enough),
/// redacted for the peer, and the agent's history, copied on first use.
class AgentEvidence final : public EvidenceSource {
public:
    struct Options {
        Millis max_age{10'000};
        std::optional<uid_t> redact_for;  // set for peers with Access::diagnostics
        ProcVisibility visibility{ProcVisibility::all};
    };
    AgentEvidence(const agent::AgentRuntime& rt, Options o) : rt_(rt), o_(o) {}

    std::shared_ptr<const SystemState> state(const std::vector<ObservationKind>& kinds, bool cgroup_focus,
                                             const CancellationToken& cancel, bool& reused) override;
    const StateHistory& history() override;
    [[nodiscard]] const EvidenceReport& report() const noexcept { return r_; }
    /// The state the answer was built from (redacted as the peer saw it), if any.
    [[nodiscard]] const std::shared_ptr<const SystemState>& used_state() const noexcept { return used_; }

private:
    const agent::AgentRuntime& rt_;
    Options o_;
    EvidenceReport r_;
    std::shared_ptr<const SystemState> used_;
    std::optional<StateHistory> hist_;
};

/// Copies `cur`, filling each cgroup's window rates from the same cgroup in
/// `prev` (the agent's previous collection): throttling over the interval
/// between two collections, without reading cpu.stat again. Returns a note
/// describing what was done.
std::string derive_cgroup_rates(SystemState& cur, const SystemState* prev);

}  // namespace osie::api
