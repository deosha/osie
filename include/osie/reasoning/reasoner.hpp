// SPDX-License-Identifier: Apache-2.0
// Deterministic, evidence-backed diagnostic reasoning.
//
// The engine separates what was *observed* from what is *hypothesised*, and
// attaches every hypothesis to an ontology mechanism (a knowledge-graph edge
// with its EdgeNature and documentation reference). Thresholds are heuristics,
// listed in docs/ARCHITECTURE.md, and are reported with each conclusion.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "osie/architecture/graph.hpp"
#include "osie/state/state.hpp"

namespace osie::json {
class Writer;
}

namespace osie::reason {

enum class FindingKind : std::uint8_t {
    observation,              // directly measured
    hypothesis,               // plausible explanation, not established
    correlation,              // co-occurring signals, no causal claim
    causal_explanation,       // mechanism documented as an OS invariant + observed preconditions
    suggested_investigation,  // what to look at next
    proposed_optimization,    // never executed by V1; informational only
};
std::string_view to_string(FindingKind k) noexcept;

enum class Certainty : std::uint8_t { none, low, limited, moderate, high };
std::string_view to_string(Certainty c) noexcept;

struct Evidence {
    std::string id;    // stable machine id, e.g. "cpu_busy_pct"
    std::string text;  // human description with value
    std::string_view source;
    std::optional<double> value;
    Validity validity{Validity::valid};
    // Observation-quality metadata (set by annotate_evidence). This is
    // *measurement* quality; model uncertainty lives only in predictions.
    std::string scope;              // "host", "process 123", "cgroup /a/b"
    std::string unit;               // "%", "cores", "bytes", "ratio", "count", ...
    std::optional<double> interval_s;  // rate measured over this window (derived values)
    std::optional<double> age_ms;      // age of the observation when the answer was built
    bool permission_limited{false};    // some inputs were not readable without privileges
};

struct Mechanism {
    std::string from, relation, to;  // concept labels
    arch::EdgeNature nature{arch::EdgeNature::hypothesized};
    float confidence{0};
    std::string explanation;
    std::string doc_ref;
};

struct Finding {
    FindingKind kind{FindingKind::observation};
    std::string code;
    std::string text;
    std::vector<std::string> evidence_ids;
    Certainty certainty{Certainty::limited};
    std::vector<Mechanism> mechanisms;
};

struct Diagnosis {
    std::string topic;       // "cpu", "memory", "slowness", "process_cpu", "process_memory"
    std::string conclusion;  // machine code, e.g. "cpu_dominated_by_process"
    std::string headline;    // one sentence
    std::vector<Evidence> evidence;
    std::vector<Finding> findings;
    std::vector<std::string> missing_evidence;
    Certainty certainty{Certainty::limited};
    std::string next_step;
    std::optional<Pid> subject_pid;
    std::vector<Pid> ranked_pids;  // processes referenced, in order (for follow-ups)

    [[nodiscard]] const Evidence* find_evidence(std::string_view id) const;
};

/// Heuristic thresholds. Exposed so tests and docs can reference them.
struct Thresholds {
    double cpu_busy_high_pct = 85.0;
    double cpu_busy_low_pct = 25.0;
    double load_per_cpu_high = 1.0;
    double dominant_share = 0.5;         // one process >= 50% of busy CPU time
    double single_thread_lo_pct = 85.0;  // ~one core fully used
    double single_thread_hi_pct = 115.0;
    double unattributed_share = 0.4;     // >=40% busy time not attributable to readable processes
    double psi_some_high = 10.0;         // avg10 %
    double psi_full_high = 5.0;
    double throttled_ratio_high = 0.1;
    double iowait_high_pct = 20.0;
    double steal_high_pct = 10.0;
    double compressed_share_high = 0.25;
    double available_share_low = 0.10;
    double swapouts_per_s_high = 1.0;
    double pageouts_per_s_high = 100.0;
    double major_faults_per_s_high = 100.0;
    double fs_full_share = 0.95;
};

/// Fills scope, unit, measurement interval, age and permission flags of
/// every evidence item from the state it was derived from.
void annotate_evidence(Diagnosis& d, const SystemState& s);

class Reasoner {
public:
    Reasoner(const arch::KnowledgeGraph& g, Platform p, Thresholds t = {}) : g_(g), platform_(p), t_(t) {}

    [[nodiscard]] Diagnosis cpu(const SystemState& s) const;
    [[nodiscard]] Diagnosis memory(const SystemState& s) const;
    [[nodiscard]] Diagnosis slowness(const SystemState& s) const;
    [[nodiscard]] Diagnosis process_cpu(const SystemState& s, Pid pid) const;
    [[nodiscard]] Diagnosis process_memory(const SystemState& s, Pid pid) const;
    /// Explains a concept from the ontology: what it is part of, what it affects.
    [[nodiscard]] Diagnosis explain_concept(std::string_view concept_key) const;

    [[nodiscard]] const Thresholds& thresholds() const noexcept { return t_; }

private:
    [[nodiscard]] std::vector<Mechanism> mechanisms_from(std::string_view concept_key, std::size_t depth = 2) const;
    [[nodiscard]] std::optional<Mechanism> mechanism(std::string_view from, std::string_view to) const;
    const arch::KnowledgeGraph& g_;
    Platform platform_;
    Thresholds t_;
};

std::string to_json(const Diagnosis& d);
/// Writes the same object into `w` (for composing larger documents).
void write_json(json::Writer& w, const Diagnosis& d);

}  // namespace osie::reason
