// SPDX-License-Identifier: Apache-2.0
// CPU world model: predicts a cgroup's CPU outcomes over the next interval
// under a hypothetical intervention (docs/WORLD_MODEL.md).
//
// A prediction is computed from observations only. Nothing here can apply an
// intervention: the model has no access to the OS, and the engine exposes
// prediction through a read-only capability.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/core/result.hpp"
#include "osie/prediction/features.hpp"

namespace osie::predict {

/// Hard limits for .osiewm files (same principles as .osiem, docs/MODEL_FORMAT.md).
namespace wm_limits {
inline constexpr std::size_t max_file_bytes = 4U << 20;
inline constexpr std::size_t max_lines = 4096;
inline constexpr std::size_t max_features = 256;
inline constexpr std::size_t max_hidden = 1024;
inline constexpr std::size_t max_targets = 16;
inline constexpr double max_abs_weight = 1e6;
}  // namespace wm_limits

struct Interval {
    double level{0};  // e.g. 0.8
    double lo{0}, hi{0};
};

/// Where the model was validated (OSIEWM2 `support_*` lines). Outside it,
/// OSIE does not predict: it falls back to observation and diagnosis.
struct SupportEnvelope {
    std::vector<std::string> arch;                 // uname machine names, e.g. aarch64 x86_64
    std::optional<std::pair<int, int>> kernel_min;  // (major, minor), inclusive
    std::optional<std::pair<int, int>> kernel_max;
    int cgroup_version{0};
    std::string throttle_semantics;  // "summed_per_cpu": cpu.stat throttled_usec summed over CPUs
    std::optional<std::pair<double, double>> capacity;  // CPU capacity after the change, cores
    std::vector<std::string> workloads;  // workload classes in the training data
    std::string model_version, data_version, calibration_version;
    int feature_schema{0};
};

/// The machine a prediction is requested on.
struct RuntimeEnvironment {
    std::string arch;            // uname -m
    std::string kernel_release;  // uname -r
    int cgroup_version{2};
};

struct TargetPrediction {
    std::string name;    // e.g. "usage_cores"
    std::string unit;
    std::string status{"validated"};  // validated | experimental (interval coverage not established)
    double current{0};   // mean over the observed history (same definition as the target)
    double expected{0};  // point prediction after the intervention, physically bounded
    std::vector<Interval> intervals;
    bool clamped{false}; // raw model output violated a physical bound and was clamped
    bool residual_applied{false};  // the learned correction was used (residual models only)
};

struct WorldPrediction {
    Intervention action;
    std::vector<TargetPrediction> targets;
    bool out_of_distribution{false};
    bool abstained{false};
    std::vector<std::string> reasons;      // why OOD / why abstained
    std::vector<std::string> missing;      // unavailable inputs
    std::string model_id;                  // provenance: model name + version
    std::size_t history_windows{0};
    /// For quota/cpuset changes: "low" when even the upper interval bound of
    /// throttled CPU time is below the model's threshold, "likely" when the
    /// lower bound is above it, "possible" otherwise, "unknown" when the
    /// prediction was withheld. Never a claim of zero risk.
    std::string throttling_assessment;
    std::vector<std::string> environment_issues;  // why this machine is outside the support envelope
};

class IWorldModel {
public:
    virtual ~IWorldModel() = default;
    /// history: oldest first. Never touches the OS.
    [[nodiscard]] virtual Result<WorldPrediction> predict(const std::vector<CgroupWindow>& history,
                                                          const Intervention& action) const = 0;
    [[nodiscard]] virtual const std::string& id() const noexcept = 0;
    [[nodiscard]] virtual std::size_t history_windows() const noexcept = 0;
    [[nodiscard]] virtual double window_seconds() const noexcept = 0;
    [[nodiscard]] virtual bool supports(ActionKind k) const noexcept = 0;
    [[nodiscard]] virtual const std::string& provenance() const noexcept = 0;
    /// Reasons `env` is outside the validated envelope (empty = compatible).
    [[nodiscard]] virtual std::vector<std::string> check_environment(const RuntimeEnvironment& env) const = 0;
    /// predict() plus the environment check: outside the envelope the
    /// prediction is withheld, never silently extrapolated.
    [[nodiscard]] Result<WorldPrediction> predict_in(const std::vector<CgroupWindow>& history, const Intervention& action,
                                                     const RuntimeEnvironment& env) const;
};

/// Linear or tiny-MLP model loaded from a .osiewm file.
class WorldModel final : public IWorldModel {
public:
    static Result<std::shared_ptr<WorldModel>> load(const std::string& path);
    static Result<std::shared_ptr<WorldModel>> load_from_memory(std::string_view text);

    [[nodiscard]] Result<WorldPrediction> predict(const std::vector<CgroupWindow>& history,
                                                  const Intervention& action) const override;
    [[nodiscard]] const std::string& id() const noexcept override { return id_; }
    [[nodiscard]] std::size_t history_windows() const noexcept override { return history_; }
    [[nodiscard]] double window_seconds() const noexcept override { return window_s_; }
    [[nodiscard]] bool supports(ActionKind k) const noexcept override;
    [[nodiscard]] std::size_t parameter_count() const noexcept;
    [[nodiscard]] const std::string& kind() const noexcept { return kind_; }
    /// One line describing what the model was trained on (environment, data).
    [[nodiscard]] const std::string& provenance() const noexcept override { return provenance_; }
    [[nodiscard]] std::vector<std::string> check_environment(const RuntimeEnvironment& env) const override;
    [[nodiscard]] const SupportEnvelope& support() const noexcept { return support_; }
    [[nodiscard]] int format_version() const noexcept { return format_; }

    struct Target {
        std::string name, unit, transform;  // transform: identity | log1p
        double lo{0}, hi{0};
        bool cap_bound{false};               // upper bound = capacity after the intervention
        std::vector<std::pair<double, double>> conformal;  // (level, half-width in target units)
        std::optional<double> abstain_width;  // abstain if the primary (lowest-level) interval is wider
        std::string status{"validated"};
        std::optional<double> residual_gate;  // apply the residual only in-distribution and when |r| <= gate
    };

private:
    WorldModel() = default;
    std::vector<double> forward(const std::vector<double>& x) const;

    std::string id_, kind_;  // kind: linear | mlp
    std::string provenance_;
    bool residual_on_mechanistic_{false};  // output = mechanistic prior + model
    std::size_t history_{6};
    double window_s_{0.5};
    std::vector<std::string> features_;
    std::vector<std::size_t> feature_index_;  // into feature_names()
    std::vector<double> mean_, scale_;
    std::vector<double> ood_lo_, ood_hi_;
    double ood_score_max_{0};                  // mean squared z-score threshold
    std::size_t hidden_{0};
    std::vector<double> w1_, b1_, w2_, b2_;    // linear: w2_/b2_ only
    std::vector<Target> targets_;
    std::vector<std::pair<ActionKind, std::pair<double, double>>> action_ranges_;
    int format_{1};
    SupportEnvelope support_;
    std::optional<double> safe_throttle_max_;  // cores; threshold of the throttling assessment
};

/// Parses "6.10.14-linuxkit" -> (6, 10).
std::optional<std::pair<int, int>> kernel_version(std::string_view release);

}  // namespace osie::predict
