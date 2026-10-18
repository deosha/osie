// SPDX-License-Identifier: Apache-2.0
// The OSIE engine: utterance -> intent frame -> authorised observations ->
// architecture graph -> reasoning -> grounded answer, with session context.
#pragma once

#include <chrono>
#include <condition_variable>
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
    /// The target is a specific process that is not running (any more): a PID
    /// with no process, or an earlier subject that exited or whose PID was
    /// reused. A definite answer, not an ambiguity.
    bool target_not_running{false};
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
    /// Keep the architecture graph's instance nodes (host, CPUs, processes,
    /// users) in step with each state answered from. Only `osie graph` reads
    /// them; reasoning uses the ontology's concept nodes. The agent API turns
    /// this off: rebuilding instances for every redacted state would cost
    /// more than the answer itself.
    bool track_instances{true};
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
    /// Where each series in `trends` came from (same order and length).
    struct SeriesOrigin {
        std::optional<ProcessRef> process;  // a process series: its identity
        std::optional<WallTime> first, last;  // the first and last point with a value
        std::uint64_t first_seq{0}, last_seq{0};
    };
    std::vector<SeriesOrigin> trend_origins;
    /// "Which process is growing?": current processes left out because their
    /// own samples were too few or too short for a trend.
    std::size_t too_new_to_judge{0};
    /// Temporal questions: the time the history window was evaluated at.
    std::optional<SteadyTime> history_evaluated_at;
    std::optional<WallTime> history_evaluated_wall;
    /// See Resolution::target_not_running.
    bool target_not_running{false};
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

/// A mutex whose lock can be awaited until a deadline. Built on std::mutex
/// and a condition variable rather than std::timed_mutex: libstdc++'s
/// timed_mutex uses pthread_mutex_clocklock, which ThreadSanitizer does not
/// model, and a sanitizer that cannot see the lock is no use.
class DeadlineMutex {
public:
    void lock() {
        std::unique_lock lk(m_);
        cv_.wait(lk, [this] { return !held_; });
        held_ = true;
    }
    bool try_lock() {
        std::lock_guard lk(m_);
        if (held_) return false;
        held_ = true;
        return true;
    }
    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& t) {
        std::unique_lock lk(m_);
        if (!cv_.wait_until(lk, t, [this] { return !held_; })) return false;
        held_ = true;
        return true;
    }
    void unlock() {
        {
            std::lock_guard lk(m_);
            held_ = false;
        }
        cv_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool held_{false};
};

/// Where an answer's evidence comes from.
///
/// The CLI samples on demand: Engine::ask(utterance) uses an internal live
/// source (the sampler, the answer cache and the engine's own history). The
/// agent API answers from the agent's published snapshot and bounded history
/// instead, through its own EvidenceSource, and never samples (src/api).
class EvidenceSource {
public:
    virtual ~EvidenceSource() = default;
    /// The state to answer from for an intent that needs `kinds`; nullptr if
    /// there is none. `cgroup_focus`: cgroup questions, which need throttling
    /// rates. `reused`: set when the state was not taken for this question.
    virtual std::shared_ptr<const SystemState> state(const std::vector<ObservationKind>& kinds, bool cgroup_focus,
                                                     const CancellationToken& cancel, bool& reused) = 0;
    /// The history temporal questions are answered from. Valid until the next
    /// call on this source.
    virtual const StateHistory& history() = 0;
};

class Engine {
public:
    Engine(std::unique_ptr<IOperatingSystem> os, std::unique_ptr<intent::IIntentParser> parser, EngineConfig cfg = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// CLI: answers with the engine's own session and on-demand observation.
    Answer ask(std::string_view utterance, const CancellationToken& cancel = {});
    /// Answers with the caller's session and evidence. The same pipeline as
    /// ask(): safety routing, validation, context, reasoning and rendering.
    /// Serialised with every other call on this engine.
    Answer ask(std::string_view utterance, SessionContext& session, EvidenceSource& evidence,
               const CancellationToken& cancel = {});
    /// As above, but gives up if the engine is not free by `deadline`
    /// (returns nullopt, having done nothing): a caller with a deadline never
    /// waits past it behind other questions.
    std::optional<Answer> ask_until(std::string_view utterance, SessionContext& session, EvidenceSource& evidence,
                                    SteadyTime deadline, const CancellationToken& cancel = {});

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
    class LiveEvidence;
    Answer answer(std::string_view utterance, SessionContext& session, EvidenceSource& evidence,
                  const CancellationToken& cancel);
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
    std::unique_ptr<LiveEvidence> live_;
    // Set for the duration of answer() (under mu_): the session and evidence
    // the current question is answered with.
    SessionContext* sess_{nullptr};
    EvidenceSource* ev_{nullptr};
    std::shared_ptr<const SystemState> graph_state_;  // the state graph_ instances were last built from
    mutable DeadlineMutex mu_;  // serialises ask(), observe_all() and record_sample()
};

/// Renders a diagnosis as plain text (shared by CLI and tests).
std::string render_diagnosis(const reason::Diagnosis& d, bool verbose);

}  // namespace osie
