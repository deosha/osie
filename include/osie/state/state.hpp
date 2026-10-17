// SPDX-License-Identifier: Apache-2.0
// State representation: a coherent, timestamped view of the system built from
// two raw samples taken `interval` apart. Rates are Validity::derived.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "osie/capabilities/capability.hpp"
#include "osie/platform/types.hpp"

namespace osie {

struct CpuUtilization {
    double user_pct{0}, system_pct{0}, nice_pct{0}, idle_pct{0};
    std::optional<double> iowait_pct;  // Linux only
    std::optional<double> steal_pct;   // Linux only
    std::vector<double> per_cpu_busy_pct;
    [[nodiscard]] double busy_pct() const noexcept { return 100.0 - idle_pct - iowait_pct.value_or(0.0); }
};

struct MemoryRates {
    std::optional<double> pageins_per_s, pageouts_per_s;
    std::optional<double> swapins_per_s, swapouts_per_s;
    std::optional<double> compressions_per_s, decompressions_per_s;
    std::optional<double> faults_per_s, major_faults_per_s;
};

struct ProcessView {
    ProcessInfo info;
    /// CPU over the sampling interval, in % of one logical CPU (like top(1)).
    std::optional<double> cpu_pct;
    /// True when the process appeared during the interval (no baseline).
    bool new_in_interval{false};
};

struct NetRates {
    std::string name;
    std::optional<double> rx_bytes_per_s, tx_bytes_per_s;
};

/// The interval a derived value (a rate or a delta) was computed over: the
/// times of the two raw readings it is the difference of.
struct DerivationWindow {
    WallTime start{}, end{};
    SteadyTime start_mono{}, end_mono{};
    [[nodiscard]] double seconds() const { return std::chrono::duration<double>(end_mono - start_mono).count(); }
    static DerivationWindow between(const Provenance& a, const Provenance& b) {
        return DerivationWindow{a.wall, b.wall, a.mono, b.mono};
    }
};

struct CgroupView {
    CgroupInfo info;
    /// When cpu.stat was read for `info` (the collector's provenance times).
    WallTime read_wall{};
    SteadyTime read_mono{};
    /// nr_throttled / nr_periods since the cgroup was created (cumulative, not
    /// a rate: one sample only, so recent throttling may be under-represented).
    std::optional<double> lifetime_throttled_ratio;
    /// Rates over a second read `window_s` later (cgroup questions only):
    /// share of periods throttled, CPUs used and CPUs' worth of throttled time.
    std::optional<double> window_s, window_throttled_ratio, window_usage_cpus, window_throttled_cpus;
    std::string window_note;  // e.g. "quota changed during sampling", "counters reset"
    /// The two cpu.stat reads the window rates were derived from, when they were.
    std::optional<DerivationWindow> window_span;
    /// The window rates span two agent collections (src/api), rather than
    /// two reads within one collection.
    bool window_across_collections{false};
};

struct Unavailable {
    ObservationKind kind;
    Error why;
};

struct SystemState {
    SteadyTime taken_at{};
    Millis interval{0};
    std::optional<Observation<SystemInfo>> system;
    std::optional<Observation<CpuTopology>> topology;
    std::optional<Observation<CpuUtilization>> cpu;
    /// The raw readings each derived observation is the difference of.
    std::optional<DerivationWindow> cpu_window, memory_rates_window, process_window;
    std::optional<Observation<LoadAverage>> load;
    std::optional<std::uint32_t> runnable_tasks;
    std::optional<double> context_switches_per_s;
    std::optional<Observation<MemoryMetrics>> memory;
    std::optional<Observation<MemoryRates>> memory_rates;
    std::optional<Observation<std::vector<ProcessView>>> processes;
    std::uint32_t processes_partial{0};
    std::uint32_t processes_vanished{0};
    std::optional<Observation<MacPowerState>> power;
    std::optional<Observation<std::vector<PressureStall>>> psi;
    std::optional<Observation<std::vector<FilesystemInfo>>> filesystems;
    std::optional<Observation<std::vector<NetInterface>>> net;
    std::vector<NetRates> net_rates;
    std::vector<CgroupView> cgroups;  // Linux: cgroups of top processes
    std::vector<Unavailable> unavailable;

    [[nodiscard]] std::vector<const ProcessView*> top_by_cpu(std::size_t n) const;
    [[nodiscard]] std::vector<const ProcessView*> top_by_memory(std::size_t n) const;
    [[nodiscard]] const ProcessView* find_pid(Pid pid) const;
    /// Case-insensitive substring match on name, then path. Sorted by memory desc.
    [[nodiscard]] std::vector<const ProcessView*> find_by_name(std::string_view name) const;
    [[nodiscard]] std::vector<const ProcessView*> children_of(Pid pid) const;
    [[nodiscard]] bool has(ObservationKind k) const;
    [[nodiscard]] const Unavailable* why_unavailable(ObservationKind k) const;
    [[nodiscard]] std::uint32_t logical_cpus() const;
};

// ---- pure derivation functions (unit-tested with synthetic counters) ------
/// Returns nullopt if counters went backwards (reset / hot-unplug).
std::optional<CpuUtilization> derive_cpu_utilization(const CpuSample& a, const CpuSample& b);
MemoryRates derive_memory_rates(const MemoryMetrics& a, const MemoryMetrics& b, double seconds);
std::vector<ProcessView> derive_process_views(const ProcessSnapshot& a, const ProcessSnapshot& b, double seconds);
std::optional<double> counter_rate(std::optional<std::uint64_t> a, std::optional<std::uint64_t> b, double seconds);

struct SampleRequest {
    std::vector<ObservationKind> kinds;
    Millis interval{500};
    std::size_t cgroup_top_n{5};  // Linux: inspect cgroups of the N busiest processes
    /// Linux: read each inspected cgroup twice, `interval` apart, for
    /// throttling rates instead of lifetime ratios.
    bool cgroup_window{false};
};

/// Rates between two cpu.stat reads of the same cgroup. Pure, for tests.
/// Leaves the window fields empty (with a note) when counters went
/// backwards, the period count did not advance, or the quota changed.
void derive_cgroup_window(CgroupView& v, const CgroupInfo& later, double seconds);

class StateSampler {
public:
    explicit StateSampler(ObservationBroker& broker) : broker_(broker) {}
    /// Never fails as a whole: each failed observation is recorded in
    /// SystemState::unavailable with its reason.
    SystemState sample(const SampleRequest& req, const CancellationToken& cancel = {});

private:
    ObservationBroker& broker_;
};

/// Thread-safe holder for the most recent state (reader/writer lock), so a
/// background refresher and the CLI can share it.
class StateCache {
public:
    void store(std::shared_ptr<const SystemState> s);
    [[nodiscard]] std::shared_ptr<const SystemState> load() const;
    /// Returns the cached state if it contains `kinds` and is younger than `budget`.
    [[nodiscard]] std::shared_ptr<const SystemState> fresh(const std::vector<ObservationKind>& kinds,
                                                           Millis budget) const;

private:
    mutable std::shared_mutex mu_;
    std::shared_ptr<const SystemState> state_;
};

}  // namespace osie
