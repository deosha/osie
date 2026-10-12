// SPDX-License-Identifier: Apache-2.0
// The OSIE engine: utterance -> intent frame -> authorised observations ->
// architecture graph -> reasoning -> grounded answer, with session context.
#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "osie/architecture/graph.hpp"
#include "osie/capabilities/capability.hpp"
#include "osie/intent/intent.hpp"
#include "osie/reasoning/reasoner.hpp"
#include "osie/state/state.hpp"

namespace osie {

struct ProcessRef {
    Pid pid{0};
    std::optional<std::uint64_t> start_time_us;
    std::string name;
};

struct EngineConfig {
    Millis sample_interval{500};
    Millis freshness_budget{2000};  // reuse cached observations younger than this
    std::size_t default_top_n{5};
    std::size_t history_limit{32};
};

struct Answer {
    intent::IntentFrame frame;
    std::optional<reason::Diagnosis> diagnosis;
    std::string text;
    Millis observation_age{0};
    bool reused_observations{false};
    double latency_ms{0};
    double parse_ms{0};
};

/// Conversational state. Deterministic; no learned components.
class SessionContext {
public:
    std::optional<ProcessRef> last_process;
    std::vector<ProcessRef> last_list;
    std::optional<intent::Intent> last_intent;
    std::deque<std::pair<std::string, intent::Intent>> history;

    /// Rewrites short elliptical follow-ups ("why?", "what about memory?")
    /// using the previous turn. Returns true if the frame was changed.
    bool apply_followup(intent::IntentFrame& f, const std::vector<intent::Token>& toks) const;

    /// Resolves reference/name/pid targets against the current state.
    /// On failure, marks the frame ambiguous with a clarification question.
    std::optional<ProcessRef> resolve(intent::IntentFrame& f, const SystemState& s) const;

    void remember(const std::string& utterance, const intent::IntentFrame& f, const std::optional<ProcessRef>& subject,
                  const std::vector<ProcessRef>& list, std::size_t limit);
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
                                                 bool& reused);
    std::string render(Answer& a, const SystemState* s, const std::optional<ProcessRef>& subject);

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
};

/// Renders a diagnosis as plain text (shared by CLI and tests).
std::string render_diagnosis(const reason::Diagnosis& d, bool verbose);

}  // namespace osie
