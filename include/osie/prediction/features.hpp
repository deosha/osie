// SPDX-License-Identifier: Apache-2.0
// Feature schema for the CPU world model (docs/WORLD_MODEL.md).
//
// The same encoder is used by the lab (tools/world_model_lab.cpp, which
// writes the training data) and by OSIE at prediction time, so the features a
// model was trained on and the features it sees are computed by one function.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/core/result.hpp"
#include "osie/platform/types.hpp"

namespace osie::predict {

inline constexpr int kFeatureSchemaVersion = 1;

/// One observation window of a cgroup: counter deltas turned into rates.
/// Fields the kernel did not provide stay empty; they are never invented.
struct CgroupWindow {
    double secs{0};
    double usage_cores{0};                   // cpu.stat usage_usec / wall time
    std::optional<double> throttled_ratio;   // nr_throttled / nr_periods (needs the cpu controller)
    std::optional<double> throttled_frac;    // throttled_usec / wall time (summed over CPUs; may exceed 1)
    std::optional<double> psi_some;          // cgroup cpu.pressure "some" share of wall time
    std::optional<double> psi_full;
    std::optional<double> quota_cores;       // cpu.max quota/period; empty = unlimited
    std::optional<double> cpuset_cpus;       // cpuset.cpus.effective
    std::optional<double> pids;              // pids.current
    std::optional<double> host_busy_frac;    // whole-machine busy share from /proc/stat
    std::optional<double> host_cpus;
};

/// Builds a window from two cgroup snapshots `secs` apart, plus optional
/// host CPU samples. Returns an error if the counters went backwards.
Result<CgroupWindow> make_window(const CgroupInfo& before, const CgroupInfo& after, double secs,
                                 const CpuSample* host_before = nullptr, const CpuSample* host_after = nullptr);

enum class ActionKind : std::uint8_t { none, cpu_quota, cpuset, offered_load };
std::string_view to_string(ActionKind k) noexcept;
std::optional<ActionKind> action_from_string(std::string_view s) noexcept;

/// A hypothetical change. Units: cpu_quota in cores (quota/period),
/// cpuset in CPUs, offered_load as a multiplier of the current load (1 = no
/// change). `none` predicts the next interval without a change.
struct Intervention {
    ActionKind kind{ActionKind::none};
    double previous{0};
    double proposed{0};
};

/// Rejects interventions that are meaningless or physically impossible
/// (negative quota, more CPUs than the host has, ...).
Result<void> validate(const Intervention& a, const CgroupWindow& current);

/// Feature names in encoding order. Prefixes mark ablation groups:
///   g_  generic telemetry (raw window statistics, raw action values)
///   m_  mechanism-aware features derived from cgroup v2 CPU bandwidth
///       semantics (capacity = min(quota, cpuset), demand estimate, ...)
///   h_  history (trend over the window sequence)
const std::vector<std::string>& feature_names();

struct EncodedFeatures {
    std::vector<double> values;         // aligned with feature_names()
    std::vector<std::string> missing;   // inputs that were unavailable (encoded as 0 + indicator)
};

/// Encodes the last `history` windows (oldest first; at least one) and the
/// intervention. Missing inputs get an explicit missingness indicator.
Result<EncodedFeatures> encode(const std::vector<CgroupWindow>& history, const Intervention& a);

/// Mechanistic estimate of the CPU demand behind the observed usage:
/// usage + throttled CPU time per second, bounded by the CPUs it may use.
/// Usage alone understates demand whenever the quota binds.
double demand_estimate(const CgroupWindow& w);

/// Effective CPU capacity after the intervention: min(quota, cpuset).
double capacity_after(const CgroupWindow& w, const Intervention& a);

/// Zero-parameter mechanistic prediction from documented cgroup v2 CPU
/// bandwidth semantics, computed from an encoded feature vector:
///   usage = min(demand, quota, cpuset); throttled = demand - min(quota, cpuset)
///   when the quota binds, else 0; throttled_ratio = 1 when it binds, else 0;
///   psi_some unchanged (no closed form). Mirrors `mechanistic()` in
///   ml/world_model/experiments.py. nullopt for an unknown target name.
std::optional<double> mechanistic_prior(std::string_view target, const std::vector<double>& encoded);

}  // namespace osie::predict
