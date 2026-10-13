// SPDX-License-Identifier: Apache-2.0
// Intent understanding.
//
// Any parser (rules, neural, hybrid) produces a ParseOutput: an intent label,
// a confidence and typed token spans. A single deterministic FrameBuilder turns
// that into a validated IntentFrame. The *required observations* and
// *capabilities* are derived from the intent by a fixed table here - a model
// can never choose what OSIE observes or what privileges it uses.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/capabilities/capability.hpp"

namespace osie::intent {

class IIntentParser;

#define OSIE_INTENTS(X)                                                                                    \
    X(top_cpu) X(top_memory) X(diagnose_cpu) X(diagnose_memory) X(diagnose_slow) X(process_cpu)            \
    X(process_memory) X(process_info) X(process_tree) X(system_info) X(cpu_topology) X(memory_status)      \
    X(disk_usage) X(network_status) X(thermal_status) X(explain_concept) X(what_if) X(container_throttle)  \
    X(action_request) X(kubernetes) X(help) X(unsupported) X(list_processes) X(cgroup_status) X(cpu_capacity)

enum class Intent : std::uint8_t {
#define OSIE_X(n) n,
    OSIE_INTENTS(OSIE_X)
#undef OSIE_X
        count_
};
std::string_view to_string(Intent i) noexcept;
std::optional<Intent> intent_from_string(std::string_view s) noexcept;
inline constexpr std::size_t kIntentCount = static_cast<std::size_t>(Intent::count_);

/// Span labels for slot tagging (BIO over tokens).
enum class SpanType : std::uint8_t { proc, pid, ref, num };
std::string_view to_string(SpanType t) noexcept;

struct Token {
    std::string text;   // original casing
    std::string lower;  // normalised form used for features
    std::size_t begin{0}, end{0};  // byte offsets into the utterance
};

/// Shared tokenizer. Must stay identical to ml/osie_ml/tokenizer.py.
std::vector<Token> tokenize(std::string_view text);

struct Span {
    SpanType type{SpanType::proc};
    std::size_t begin{0}, end{0};  // token indices [begin, end)
};

enum class ActionType : std::uint8_t { none, terminate, set_priority, set_affinity, kernel_parameter, cgroup_limit,
                                       restart_service, network_config, kubernetes_patch, generic_change,
                                       file_delete, firewall_config, container_control };
std::string_view to_string(ActionType a) noexcept;
/// Capability id an action request maps to ("act.process.terminate", ...).
std::string_view action_capability(ActionType a) noexcept;

/// How an utterance relates to a state-changing action. Assessed
/// deterministically, before and independently of any parser, so a learned
/// model can neither turn an instruction into a question nor the reverse.
enum class SafetyMode : std::uint8_t {
    none,              // no action is mentioned
    directive,         // the user asks OSIE to perform an action -> refused
    explanation,       // "how does kill -9 work?"
    past_event,        // "why was process 123 killed?"
    hypothetical,      // "what if I halve the quota?", "predict ..."
    quoted_mention,    // the action appears only inside quotes
};
std::string_view to_string(SafetyMode m) noexcept;

struct SafetyAssessment {
    SafetyMode mode{SafetyMode::none};
    ActionType action{ActionType::none};  // set for directives
    bool override_attempt{false};         // "ignore your rules", "admin mode", "I authorize you" ...
    std::string cue;                      // words that decided the mode (shown in refusals and audits)
    std::string pid;                      // bare PID operand of a directive ("kill -9 4242")
};

/// Safety-intent detection over clauses. `text` is the raw utterance (quote
/// marks and punctuation are not tokens); `toks` its normalised tokens.
SafetyAssessment assess_safety(std::string_view text, const std::vector<Token>& toks);

/// Deterministic normalisation of `Token::lower` (abbreviations, common
/// misspellings of OSIE vocabulary). `text` and offsets are unchanged, so
/// names are always reported as the user wrote them.
void normalize(std::vector<Token>& toks);

struct ParseOutput {
    Intent intent{Intent::unsupported};
    float confidence{0.0F};
    std::vector<Span> spans;
    std::string parser;  // "rules", "neural", "hybrid:<which>"
    SafetyAssessment safety;  // set by understand(), never by a parser
};

enum class TargetKind : std::uint8_t { host, process_name, pid, reference, none };
std::string_view to_string(TargetKind k) noexcept;

struct Target {
    TargetKind kind{TargetKind::host};
    std::string value;              // name or pid digits; reference phrase
    std::optional<int> ordinal;     // "the second one" -> 2 (1-based)
};


/// A question about change over time ("has CPU usage increased?", "when did
/// memory pressure become elevated?", "is the system recovering?"). Detected
/// deterministically from cue words for every parser; answered only from
/// recorded history, never from a single snapshot.
enum class TemporalKind : std::uint8_t { trend, onset, recovery };
enum class TemporalMetric : std::uint8_t { cpu, memory, memory_pressure, overall };
std::string_view to_string(TemporalKind k) noexcept;
std::string_view to_string(TemporalMetric m) noexcept;

struct TemporalQuery {
    TemporalKind kind{TemporalKind::trend};
    TemporalMetric metric{TemporalMetric::overall};
    std::optional<std::int64_t> window_s;  // "in the last five minutes" -> 300
    bool rank_processes{false};            // "which process has been growing?"
};

/// Temporal cues in `toks`, or nullopt. Exposed for tests.
std::optional<TemporalQuery> detect_temporal(const std::vector<Token>& toks);

/// A hypothetical change named in a what-if question ("if nginx gets another
/// CPU", "halve the quota", "limit postgres to 1 core").
struct HypotheticalChange {
    enum class Op : std::uint8_t { set, add, multiply };
    enum class Resource : std::uint8_t { cpu_quota, cpuset };
    Resource resource{Resource::cpu_quota};
    Op op{Op::set};
    double value{0};
};

struct IntentFrame {
    Intent intent{Intent::unsupported};
    float confidence{0.0F};
    std::string parser;
    Target target;
    std::vector<std::string> entities;  // "cpu", "memory", "processes", ...
    std::optional<int> top_n;
    ActionType action{ActionType::none};
    std::string concept_key;            // explain_concept / what_if subject
    bool read_only{true};               // V1 constraint, always true
    bool ambiguous{false};
    std::string clarification;          // question to ask when ambiguous
    std::string unsupported_reason;
    std::vector<ObservationKind> observations;            // supported & required
    std::vector<std::string> unavailable_observations;    // required but unsupported here
    std::vector<std::string> capabilities;                // capability ids to be invoked
    std::vector<std::string> validation_errors;           // empty = schema-valid
    std::optional<TemporalQuery> temporal;
    SafetyMode safety{SafetyMode::none};
    bool override_attempt{false};
    std::optional<HypotheticalChange> change;  // what_if only
};

/// Concept lexicon: maps phrases ("run queue", "memory compression", ...) to ontology keys.
std::optional<std::string> find_concept(const std::vector<Token>& toks);
ActionType find_action(const std::vector<Token>& toks);
std::optional<int> parse_ordinal(std::string_view word);

/// Deterministic: ParseOutput + tokens -> IntentFrame (entities, target, observations).
IntentFrame build_frame(const ParseOutput& p, const std::vector<Token>& toks, Platform platform,
                        const ObservationSet& supported, const CapabilityRegistry& reg);

/// Utterances longer than this are truncated before parsing.
inline constexpr std::size_t kMaxUtteranceBytes = 4096;

/// The staged pipeline used by the engine and the evaluators:
/// normalise -> safety assessment -> parser -> frame -> safety routing.
/// `toks_out` receives the normalised tokens.
IntentFrame understand(std::string_view text, const IIntentParser& parser, Platform platform,
                       const ObservationSet& supported, const CapabilityRegistry& reg,
                       std::vector<Token>* toks_out = nullptr);

/// Schema validation; returns violations.
std::vector<std::string> validate(const IntentFrame& f);

std::string to_json(const IntentFrame& f);

class IIntentParser {
public:
    virtual ~IIntentParser() = default;
    virtual ParseOutput parse(const std::vector<Token>& toks) const = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

/// Deterministic keyword/pattern parser. No learned parameters.
class RuleParser final : public IIntentParser {
public:
    ParseOutput parse(const std::vector<Token>& toks) const override;
    [[nodiscard]] std::string_view name() const noexcept override { return "rules"; }
    /// Slot extraction only (exposed so the neural path can be compared/combined).
    static std::vector<Span> extract_spans(const std::vector<Token>& toks);
};

}  // namespace osie::intent
