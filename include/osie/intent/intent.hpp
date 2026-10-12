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

#define OSIE_INTENTS(X)                                                                                    \
    X(top_cpu) X(top_memory) X(diagnose_cpu) X(diagnose_memory) X(diagnose_slow) X(process_cpu)            \
    X(process_memory) X(process_info) X(process_tree) X(system_info) X(cpu_topology) X(memory_status)      \
    X(disk_usage) X(network_status) X(thermal_status) X(explain_concept) X(what_if) X(container_throttle)  \
    X(action_request) X(kubernetes) X(help) X(unsupported)

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

struct ParseOutput {
    Intent intent{Intent::unsupported};
    float confidence{0.0F};
    std::vector<Span> spans;
    std::string parser;  // "rules", "neural", "hybrid:<which>"
};

enum class TargetKind : std::uint8_t { host, process_name, pid, reference, none };
std::string_view to_string(TargetKind k) noexcept;

struct Target {
    TargetKind kind{TargetKind::host};
    std::string value;              // name or pid digits; reference phrase
    std::optional<int> ordinal;     // "the second one" -> 2 (1-based)
};

enum class ActionType : std::uint8_t { none, terminate, set_priority, set_affinity, kernel_parameter, cgroup_limit,
                                       restart_service, network_config, kubernetes_patch, generic_change };
std::string_view to_string(ActionType a) noexcept;
/// Capability id an action request maps to ("act.process.terminate", ...).
std::string_view action_capability(ActionType a) noexcept;

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
};

/// Concept lexicon: maps phrases ("run queue", "memory compression", ...) to ontology keys.
std::optional<std::string> find_concept(const std::vector<Token>& toks);
ActionType find_action(const std::vector<Token>& toks);
std::optional<int> parse_ordinal(std::string_view word);

/// Deterministic: ParseOutput + tokens -> IntentFrame (entities, target, observations).
IntentFrame build_frame(const ParseOutput& p, const std::vector<Token>& toks, Platform platform,
                        const ObservationSet& supported, const CapabilityRegistry& reg);

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
