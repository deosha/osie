// SPDX-License-Identifier: Apache-2.0
// The OSIE engine: utterance -> intent frame -> authorised observations ->
// architecture graph -> reasoning -> grounded answer, with session context.
#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "osie/architecture/graph.hpp"
#include "osie/capabilities/capability.hpp"
#include "osie/intent/intent.hpp"
#include "osie/prediction/world_model.hpp"
#include "osie/reasoning/reasoner.hpp"
#include "osie/state/history.hpp"
#include "osie/state/state.hpp"

namespace osie {

struct ProcessRef {
    Pid pid{0};
    std::optional<std::uint64_t> start_time_us;
    std::string name;
};

/// Outcome of resolving a process target against the current state.
struct Resolution {
    std::optional<ProcessRef> subject;
    /// Every process matching a name target, ranked by the metric the question
    /// is about. More than one entry means the name is ambiguous: OSIE then
    /// says so instead of silently picking one instance.
    std::vector<ProcessRef> group;
    /// Processes whose name merely contains the query, when exact matches exist
    /// (reported, not included in `group`).
    std::size_t partial_matches{0};
};

struct EngineConfig {
    Millis sample_interval{500};
    Millis freshness_budget{2000};  // reuse cached observations younger than this
    std::size_t default_top_n{5};
    std::size_t history_limit{32};
    /// References such as "it" or "the second one" expire after this long:
    /// an old answer is not silently reused as the subject of a new question.
    Millis context_ttl{10 * 60 * 1000};
    HistoryConfig history{};
    /// Cgroups inspected for cgroup_status questions (of the N busiest processes).
    std::size_t cgroup_scan_n{40};
    /// Predictions are experimental and off unless explicitly enabled
    /// (`osie predict --experimental` or OSIE_EXPERIMENTAL_PREDICT=1).
    bool experimental_prediction{false};
};

struct Answer {
    intent::IntentFrame frame;
    std::optional<reason::Diagnosis> diagnosis;
    std::string text;
    Millis observation_age{0};
    bool reused_observations{false};
    double latency_ms{0};
    double parse_ms{0};
    /// Temporal questions: the series that were analysed (label, result).
    std::vector<std::pair<std::string, TrendResult>> trends;
};

/// Conversational state. Deterministic; no learned components.
class SessionContext {
public:
    std::optional<ProcessRef> last_process;
    std::vector<ProcessRef> last_list;
    std::optional<intent::Intent> last_intent;
    std::deque<std::pair<std::string, intent::Intent>> history;
    SteadyTime updated_at{};  // when last_process / last_list were set
    /// The last refused action. A following "yes, do it" or "I authorize it"
    /// is recognised as insisting on it and is refused again (never executed).
    std::optional<intent::ActionType> last_refused_action;
    intent::Target last_refused_target;

    /// Rewrites short elliptical follow-ups ("why?", "what about memory?")
    /// using the previous turn. Returns true if the frame was changed.
    bool apply_followup(intent::IntentFrame& f, const std::vector<intent::Token>& toks) const;

    /// Resolves reference/name/pid targets against the current state.
    /// Identity is (pid, start time): a reused PID is a different process.
    /// On failure, marks the frame ambiguous with a clarification question.
    Resolution resolve(intent::IntentFrame& f, const SystemState& s, SteadyTime now, Millis ttl) const;

    void remember(const std::string& utterance, const intent::IntentFrame& f, const std::optional<ProcessRef>& subject,
                  const std::vector<ProcessRef>& list, std::size_t limit, SteadyTime now);
};

/// What a hypothetical-intervention question is about.
struct PredictTarget {
    std::optional<Pid> pid;
    std::string process_name;  // resolved like other process targets
    std::string cgroup;        // explicit cgroup path (Linux), e.g. /system.slice/nginx.service
};

struct PredictionAnswer {
    std::string text;
    std::optional<predict::WorldPrediction> prediction;  // set when the model ran
    std::string cgroup;
    bool answered{false};  // false: unsupported, insufficient evidence, or abstained
};

class Engine {
public:
    Engine(std::unique_ptr<IOperatingSystem> os, std::unique_ptr<intent::IIntentParser> parser, EngineConfig cfg = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Answer ask(std::string_view utterance, const CancellationToken& cancel = {});

    /// Snapshot of everything (used by `osie snapshot`).
    std::shared_ptr<const SystemState> observe_all(const CancellationToken& cancel = {});

    /// Takes one sample of CPU, memory and processes and records it in the
    /// bounded history (used by the opt-in --watch sampler). Thread-safe with
    /// respect to ask().
    void record_sample(const CancellationToken& cancel = {});

    /// Read-only prediction of a cgroup's CPU outcomes under a hypothetical
    /// intervention (Linux, docs/WORLD_MODEL.md). Observes the cgroup for the
    /// model's history window, then evaluates the model. Never applies the
    /// intervention: there is no code path that writes to the cgroup.
    PredictionAnswer predict(const PredictTarget& target, predict::ActionKind kind, double proposed,
                             const CancellationToken& cancel = {});
    void set_world_model(std::shared_ptr<const predict::IWorldModel> m) { world_model_ = std::move(m); }
    [[nodiscard]] const predict::IWorldModel* world_model() const noexcept { return world_model_.get(); }
    [[nodiscard]] std::size_t history_size() const;
    [[nodiscard]] std::size_t history_bytes() const;

    [[nodiscard]] const CapabilityRegistry& registry() const noexcept { return reg_; }
    [[nodiscard]] AuditLog& audit() noexcept { return audit_; }
    [[nodiscard]] const arch::KnowledgeGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] IOperatingSystem& os() noexcept { return *os_; }
    [[nodiscard]] const SessionContext& session() const noexcept { return session_; }
    [[nodiscard]] intent::IIntentParser& parser() noexcept { return *parser_; }
    void set_parser(std::unique_ptr<intent::IIntentParser> p) { parser_ = std::move(p); }
    /// Parses without observing (used by `osie parse` and evaluation).
    intent::IntentFrame interpret(std::string_view utterance) const;

private:
    std::shared_ptr<const SystemState> state_for(const std::vector<ObservationKind>& kinds, const CancellationToken& c,
                                                 bool& reused, bool cgroup_focus = false);
    std::string render_capacity(const intent::IntentFrame& f, const std::optional<ProcessRef>& subject);
    std::string render(Answer& a, const SystemState* s, const Resolution& r);
    std::string render_group(const SystemState& s, const Resolution& r, const intent::IntentFrame& f) const;
    std::string render_temporal(Answer& a, const Resolution& r);
    std::vector<std::pair<Pid, std::optional<std::uint64_t>>> pinned_identities() const;

    std::unique_ptr<IOperatingSystem> os_;
    std::unique_ptr<intent::IIntentParser> parser_;
    EngineConfig cfg_;
    CapabilityRegistry reg_;
    SafetyPolicy policy_;
    AuditLog audit_;
    ObservationBroker broker_;
    StateSampler sampler_;
    StateCache cache_;
    arch::KnowledgeGraph graph_;
    reason::Reasoner reasoner_;
    SessionContext session_;
    StateHistory history_;
    std::shared_ptr<const predict::IWorldModel> world_model_;
    mutable std::mutex mu_;  // serialises ask(), observe_all() and record_sample()
};

/// Renders a diagnosis as plain text (shared by CLI and tests).
std::string render_diagnosis(const reason::Diagnosis& d, bool verbose);

}  // namespace osie
