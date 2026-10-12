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

struct TargetPrediction {
    std::string name;    // e.g. "usage_cores"
    std::string unit;
    double current{0};   // mean over the observed history (same definition as the target)
    double expected{0};  // point prediction after the intervention, physically bounded
    std::vector<Interval> intervals;
    bool clamped{false}; // raw model output violated a physical bound and was clamped
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

    struct Target {
        std::string name, unit, transform;  // transform: identity | log1p
        double lo{0}, hi{0};
        bool cap_bound{false};               // upper bound = capacity after the intervention
        std::vector<std::pair<double, double>> conformal;  // (level, half-width in target units)
        std::optional<double> abstain_width;  // abstain if the primary (lowest-level) interval is wider
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
};

}  // namespace osie::predict
