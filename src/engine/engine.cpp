// SPDX-License-Identifier: Apache-2.0
#include "osie/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <thread>
#include <tuple>

#include "osie/architecture/model.hpp"
#include "osie/util/format.hpp"

namespace osie {
using intent::Intent;
using intent::TargetKind;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool has_tok(const std::vector<intent::Token>& t, std::string_view w) {
    return std::any_of(t.begin(), t.end(), [&](const intent::Token& x) { return x.lower == w; });
}

/// Short confirmations or insistence after a refusal ("yes, do it", "go
/// ahead", "I authorize it", "just do it already", "confirm").
bool insists_on_action(const std::vector<intent::Token>& t) {
    static const std::set<std::string_view> kStrong = {"do", "go", "yes", "yeah", "yep", "proceed", "confirm",
                                                       "confirmed", "approve", "approved", "authorize", "authorise",
                                                       "authorized", "authorised", "sure", "ok", "okay", "execute",
                                                       "permission", "allowed", "continue", "y"};
    static const std::set<std::string_view> kWeak = {"it", "ahead", "just", "now", "i", "you", "have", "my", "that",
                                                     "then", "the", "to", "can", "please", "already", "said", "told",
                                                     "am", "admin", "root", "as", "really", "mean", "seriously",
                                                     "anyway", "still", "want", "again", "this", "one", "is", "are",
                                                     "fine", "with", "me", "give", "for", "so", "and", "action",
                                                     "request", "of", "course", "please", "right", "away"};
    if (t.empty() || t.size() > 10) return false;
    bool strong = false;
    for (const auto& x : t) {
        if (kStrong.count(x.lower)) strong = true;
        else if (!kWeak.count(x.lower)) return false;
    }
    return strong;
}

bool is_process_intent(Intent i) {
    return i == Intent::process_cpu || i == Intent::process_memory || i == Intent::process_info ||
           i == Intent::process_tree;
}

ProcessRef ref_of(const ProcessView& p) { return ProcessRef{p.info.pid, p.info.start_time_us, p.info.name}; }

std::string lower_ascii(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string kind_label(reason::FindingKind k) {
    switch (k) {
        case reason::FindingKind::observation: return "Observed";
        case reason::FindingKind::hypothesis: return "Hypothesis";
        case reason::FindingKind::correlation: return "Correlation";
        case reason::FindingKind::causal_explanation: return "Explanation";
        case reason::FindingKind::suggested_investigation: return "Investigate";
        case reason::FindingKind::proposed_optimization: return "Proposal";
    }
    return "?";
}

}  // namespace

// ------------------------------------------------------------- session ---

bool SessionContext::apply_followup(intent::IntentFrame& f, const std::vector<intent::Token>& toks) const {
    if (!last_intent || toks.size() > 6) return false;
    const bool mem = has_tok(toks, "memory") || has_tok(toks, "ram");
    const bool cpu = has_tok(toks, "cpu") || has_tok(toks, "processor");
    const bool about = has_tok(toks, "about") || has_tok(toks, "and") || toks.size() <= 3;
    const Intent prev = *last_intent;
    // "why?" / "why is that?" after a process-focused answer.
    if (has_tok(toks, "why") && toks.size() <= 3 && last_process) {
        if (prev == Intent::top_memory || prev == Intent::process_memory || prev == Intent::diagnose_memory) {
            f.intent = Intent::process_memory;
        } else if (prev == Intent::top_cpu || prev == Intent::process_cpu || prev == Intent::diagnose_cpu) {
            f.intent = Intent::process_cpu;
        } else {
            return false;
        }
        f.target = intent::Target{TargetKind::reference, "previous process", std::nullopt};
        f.ambiguous = false;
        f.clarification.clear();
        return true;
    }
    // "what about memory?" keeps the previous intent family and target.
    if (about && (mem != cpu)) {
        Intent next = prev;
        if (prev == Intent::top_cpu && mem) next = Intent::top_memory;
        else if (prev == Intent::top_memory && cpu) next = Intent::top_cpu;
        else if (prev == Intent::process_cpu && mem) next = Intent::process_memory;
        else if (prev == Intent::process_memory && cpu) next = Intent::process_cpu;
        else if (prev == Intent::diagnose_cpu && mem) next = Intent::diagnose_memory;
        else if (prev == Intent::diagnose_memory && cpu) next = Intent::diagnose_cpu;
        if (next == prev || next == f.intent) return false;
        f.intent = next;
        if (is_process_intent(next)) {
            f.target = intent::Target{TargetKind::reference, "previous process", std::nullopt};
            f.ambiguous = false;
            f.clarification.clear();
        }
        return true;
    }
    return false;
}

Resolution SessionContext::resolve(intent::IntentFrame& f, const SystemState& s, SteadyTime now, Millis ttl) const {
    Resolution res;
    auto fail = [&f, &res](std::string q) {
        f.ambiguous = true;
        f.clarification = std::move(q);
        return res;
    };
    switch (f.target.kind) {
        case TargetKind::pid: {
            const Pid pid = static_cast<Pid>(std::stol(f.target.value));
            if (const auto* p = s.find_pid(pid)) {
                res.subject = ref_of(*p);
                return res;
            }
            res.target_not_running = true;
            return fail("No running process has PID " + f.target.value + " (it may have exited).");
        }
        case TargetKind::process_name: {
            const auto matches = s.find_by_name(f.target.value);
            if (matches.empty()) return fail("I can't find a running process matching '" + f.target.value + "'.");
            const std::string needle = lower_ascii(f.target.value);
            std::vector<const ProcessView*> exact, partial;
            for (const auto* m : matches) (lower_ascii(m->info.name) == needle ? exact : partial).push_back(m);
            auto group = exact.empty() ? partial : exact;
            res.partial_matches = exact.empty() ? 0 : partial.size();
            // Rank by the metric the question is about (find_by_name ranks by memory).
            if (f.intent == Intent::process_cpu) {
                std::stable_sort(group.begin(), group.end(), [](const ProcessView* a, const ProcessView* b) {
                    return a->cpu_pct.value_or(-1) > b->cpu_pct.value_or(-1);
                });
            }
            for (const auto* m : group) res.group.push_back(ref_of(*m));
            if (group.size() == 1 || f.intent == Intent::process_cpu || f.intent == Intent::process_memory) {
                // CPU/memory questions about a name with several instances are
                // answered for the whole group, with details for the top one.
                res.subject = res.group.front();
                return res;
            }
            std::string q = std::to_string(group.size()) + " processes match '" + f.target.value + "':\n";
            for (std::size_t i = 0; i < group.size() && i < 8; ++i)
                q += "  " + std::to_string(i + 1) + ". " + group[i]->info.name + " (pid " + std::to_string(group[i]->info.pid) + ")\n";
            if (group.size() > 8) q += "  ... and " + std::to_string(group.size() - 8) + " more\n";
            q += "Which one? Give a PID, or say e.g. 'the second one'.";
            return fail(std::move(q));
        }
        case TargetKind::reference: {
            const ProcessRef* r = nullptr;
            if (f.target.ordinal && !last_list.empty()) {
                const int o = *f.target.ordinal;
                if (o == -1) r = &last_list.back();
                else if (o >= 1 && static_cast<std::size_t>(o) <= last_list.size()) r = &last_list[static_cast<std::size_t>(o - 1)];
                else return fail("My last list had " + std::to_string(last_list.size()) + " entries; which one do you mean?");
            } else if (last_process) {
                r = &*last_process;
            } else if (!last_list.empty()) {
                r = &last_list.front();
            }
            if (r == nullptr) return fail("Which process do you mean? I have no earlier process to refer to.");
            if (now - updated_at > ttl) {
                const auto mins = std::chrono::duration_cast<std::chrono::minutes>(now - updated_at).count();
                return fail("My earlier answer about " + r->name + " is " + std::to_string(mins) +
                            " minutes old, so I won't assume you still mean it. Which process (name or PID)?");
            }
            const auto* p = s.find_pid(r->pid);
            // Same PID is the same process only if the start time matches; when
            // either start time is unknown, fall back to comparing the name.
            const bool reused = p != nullptr && (r->start_time_us && p->info.start_time_us
                                                     ? *r->start_time_us != *p->info.start_time_us
                                                     : p->info.name != r->name);
            if (p == nullptr || reused) {
                res.target_not_running = true;
                return fail(r->name + " (pid " + std::to_string(r->pid) + ") is no longer running" +
                            (reused ? " (that PID now belongs to " + p->info.name + ")." : "."));
            }
            res.subject = ref_of(*p);
            return res;
        }
        case TargetKind::host:
        case TargetKind::none: break;
    }
    return res;
}

void SessionContext::remember(const std::string& utterance, const intent::IntentFrame& f,
                              const std::optional<ProcessRef>& subject, const std::vector<ProcessRef>& list,
                              std::size_t limit, SteadyTime now) {
    last_intent = f.intent;
    if (f.intent == intent::Intent::action_request) {
        last_refused_action = f.action;
        last_refused_target = f.target;
        if (subject) last_refused_target = intent::Target{TargetKind::pid, std::to_string(subject->pid), std::nullopt};
    } else {
        last_refused_action.reset();
    }
    if (subject || !list.empty()) updated_at = now;
    if (subject) last_process = subject;
    if (!list.empty()) {
        last_list = list;
        if (!subject) last_process = list.front();
    }
    history.emplace_back(utterance, f.intent);
    while (history.size() > limit) history.pop_front();
}

// -------------------------------------------------------------- engine ---

/// The CLI's evidence: sampled on demand (reusing a fresh cached state), and
/// recorded in the engine's own history.
class Engine::LiveEvidence final : public EvidenceSource {
public:
    explicit LiveEvidence(Engine& e) : e_(e) {}
    std::shared_ptr<const SystemState> state(const std::vector<ObservationKind>& kinds, bool cgroup_focus,
                                             const CancellationToken& cancel, bool& reused) override {
        return e_.state_for(kinds, cancel, reused, cgroup_focus);
    }
    const StateHistory& history() override { return e_.history_; }

private:
    Engine& e_;
};

Engine::Engine(std::unique_ptr<IOperatingSystem> os, std::unique_ptr<intent::IIntentParser> parser, EngineConfig cfg)
    : os_(std::move(os)),
      parser_(std::move(parser)),
      cfg_(cfg),
      reg_(CapabilityRegistry::builtin()),
      broker_(*os_, reg_, policy_, audit_),
      sampler_(broker_),
      reasoner_(graph_, os_->platform()),
      history_(cfg_.history),
      live_(std::make_unique<LiveEvidence>(*this)) {
    arch::load_ontology(graph_);
}

Engine::~Engine() = default;

intent::IntentFrame Engine::interpret(std::string_view utterance) const {
    return intent::understand(utterance, *parser_, os_->platform(), os_->supported(), reg_);
}

std::shared_ptr<const SystemState> Engine::state_for(const std::vector<ObservationKind>& kinds,
                                                     const CancellationToken& c, bool& reused, bool cgroup_focus) {
    reused = false;
    if (kinds.empty()) return nullptr;
    // Cgroup questions need throttling *rates* over many cgroups, which an
    // ordinary cached sample does not have.
    if (auto s = cgroup_focus ? nullptr : cache_.fresh(kinds, cfg_.freshness_budget)) {
        reused = true;
        return s;
    }
    SampleRequest req;
    req.kinds = kinds;
    req.interval = cfg_.sample_interval;
    if (cgroup_focus) {
        req.cgroup_top_n = cfg_.cgroup_scan_n;
        req.cgroup_window = true;
    }
    // Always include system info + topology: cheap and needed for context.
    for (auto k : {ObservationKind::system_info, ObservationKind::cpu_topology})
        if (std::find(req.kinds.begin(), req.kinds.end(), k) == req.kinds.end()) req.kinds.push_back(k);
    auto st = std::make_shared<SystemState>(sampler_.sample(req, c));
    arch::update_instances(graph_, *st, os_->platform());
    graph_state_ = st;
    cache_.store(st);
    history_.record(*st, pinned_identities());
    return st;
}

std::vector<std::pair<Pid, std::optional<std::uint64_t>>> Engine::pinned_identities() const {
    std::vector<std::pair<Pid, std::optional<std::uint64_t>>> v;
    if (session_.last_process) v.emplace_back(session_.last_process->pid, session_.last_process->start_time_us);
    return v;
}

void Engine::record_sample(const CancellationToken& c) {
    std::lock_guard lk(mu_);
    SampleRequest req;
    req.interval = cfg_.sample_interval;
    req.cgroup_top_n = 0;
    for (auto k : {ObservationKind::cpu_times, ObservationKind::memory, ObservationKind::memory_pressure_level,
                   ObservationKind::pressure_stall, ObservationKind::process_list})
        if (os_->supported().contains(k)) req.kinds.push_back(k);
    // Not stored in the answer cache: the architecture graph is only updated
    // for states that answers are built from.
    const auto st = sampler_.sample(req, c);
    history_.record(st, pinned_identities());
}

std::size_t Engine::history_size() const {
    std::lock_guard lk(mu_);
    return history_.size();
}

std::size_t Engine::history_bytes() const {
    std::lock_guard lk(mu_);
    return history_.approx_bytes();
}

std::shared_ptr<const SystemState> Engine::observe_all(const CancellationToken& c) {
    std::lock_guard lk(mu_);
    std::vector<ObservationKind> all;
    for (auto k : os_->supported().list()) all.push_back(k);
    bool reused = false;
    return state_for(all, c, reused);
}

Answer Engine::ask(std::string_view utterance, const CancellationToken& cancel) {
    std::lock_guard lk(mu_);
    return answer(utterance, session_, *live_, cancel);
}

Answer Engine::ask(std::string_view utterance, SessionContext& session, EvidenceSource& evidence,
                   const CancellationToken& cancel) {
    std::lock_guard lk(mu_);
    return answer(utterance, session, evidence, cancel);
}

std::optional<Answer> Engine::ask_until(std::string_view utterance, SessionContext& session, EvidenceSource& evidence,
                                        SteadyTime deadline, const CancellationToken& cancel) {
    std::unique_lock lk(mu_, std::defer_lock);
    if (!lk.try_lock_until(deadline)) return std::nullopt;
    return answer(utterance, session, evidence, cancel);
}

Answer Engine::answer(std::string_view utterance, SessionContext& session, EvidenceSource& evidence,
                      const CancellationToken& cancel) {
    // The rendering helpers read the session and evidence of this question.
    struct Bind {
        Engine& e;
        ~Bind() { e.sess_ = nullptr, e.ev_ = nullptr; }
    } bind{*this};
    sess_ = &session;
    ev_ = &evidence;
    const auto t0 = std::chrono::steady_clock::now();
    Answer a;
    std::vector<intent::Token> toks;
    a.frame = intent::understand(utterance, *parser_, os_->platform(), os_->supported(), reg_, &toks);
    a.parse_ms = ms_since(t0);

    // "yes, do it", "go ahead", "I authorize it" right after a refusal insist
    // on the refused action: it is refused again, with the same capability.
    if (a.frame.intent != Intent::action_request && session.last_refused_action && insists_on_action(toks)) {
        auto& f = a.frame;
        f.intent = Intent::action_request;
        f.action = *session.last_refused_action;
        f.target = session.last_refused_target;
        f.safety = intent::SafetyMode::directive;
        f.temporal.reset();
        f.ambiguous = false;
        f.clarification.clear();
        f.observations.clear();
        f.unavailable_observations.clear();
        f.capabilities.clear();
        f.entities.clear();
        f.concept_key.clear();
        f.parser += "+insistence";
        if (const auto cap = intent::action_capability(f.action); !cap.empty()) f.capabilities.emplace_back(cap);
    }

    // A temporal follow-up that names no metric ("has its usage increased?")
    // continues the metric of the previous turn.
    if (a.frame.temporal && a.frame.temporal->metric == intent::TemporalMetric::overall &&
        a.frame.target.kind == TargetKind::reference && session.last_intent) {
        const auto prev = *session.last_intent;
        if (prev == Intent::top_memory || prev == Intent::process_memory || prev == Intent::diagnose_memory ||
            prev == Intent::memory_status) {
            a.frame.intent = Intent::process_memory;
            a.frame.temporal->metric = intent::TemporalMetric::memory;
        } else if (prev == Intent::top_cpu || prev == Intent::process_cpu || prev == Intent::diagnose_cpu) {
            a.frame.intent = Intent::process_cpu;
            a.frame.temporal->metric = intent::TemporalMetric::cpu;
        }
    }

    // Context: short follow-ups and references.
    if (!a.frame.temporal && session.apply_followup(a.frame, toks)) {
        intent::ParseOutput ctx;
        ctx.intent = a.frame.intent;
        ctx.confidence = a.frame.confidence;
        ctx.parser = a.frame.parser + "+context";
        a.frame = intent::build_frame(ctx,
                                      toks, os_->platform(), os_->supported(), reg_);
        if (is_process_intent(a.frame.intent)) {
            a.frame.target = intent::Target{TargetKind::reference, "previous process", std::nullopt};
            a.frame.ambiguous = false;
            a.frame.clarification.clear();
        }
    }

    // A frame that fails validation is never acted on: nothing is observed and
    // the answer says so. (Ambiguity is not a validation failure.)
    // Action requests skip this: they are always refused and audited in render().
    if (auto errs = intent::validate(a.frame); !errs.empty() && a.frame.intent != Intent::action_request) {
        a.frame.validation_errors = std::move(errs);
        a.text = "I could not interpret that request safely, so I did nothing (" + a.frame.validation_errors.front() +
                 ").\n";
        session.remember(std::string(utterance), a.frame, std::nullopt, {}, cfg_.history_limit,
                          std::chrono::steady_clock::now());
        a.latency_ms = ms_since(t0);
        return a;
    }

    bool reused = false;
    std::shared_ptr<const SystemState> st;
    if (!a.frame.ambiguous || is_process_intent(a.frame.intent)) {
        const bool cgroup_focus = os_->platform() == Platform::linux_os &&
                                  (a.frame.intent == Intent::cgroup_status || a.frame.intent == Intent::container_throttle);
        st = evidence.state(a.frame.observations, cgroup_focus, cancel, reused);
        // Instances in the architecture graph follow the state being explained.
        if (cfg_.track_instances && st && st != graph_state_) {
            arch::update_instances(graph_, *st, os_->platform());
            graph_state_ = st;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    Resolution res;
    if (st && a.frame.target.kind != TargetKind::host && a.frame.target.kind != TargetKind::none) {
        res = session.resolve(a.frame, *st, now, cfg_.context_ttl);
        if (res.subject) {
            a.frame.ambiguous = false;
            a.frame.clarification.clear();
        }
    }
    const auto& subject = res.subject;
    a.target_not_running = res.target_not_running;
    a.frame.validation_errors = intent::validate(a.frame);
    a.reused_observations = reused;
    if (st) a.observation_age = std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - st->taken_at);

    a.text = (a.frame.temporal && st && !(a.frame.ambiguous && !subject)) ? render_temporal(a, res)
                                                                         : render(a, st.get(), res);

    // Ordinals in the next turn refer to the last list shown: a ranked answer,
    // or the instances of an ambiguous name.
    std::vector<ProcessRef> list;
    if (res.group.size() > 1) {
        list = res.group;
    } else if (a.diagnosis && st) {
        for (Pid p : a.diagnosis->ranked_pids)
            if (const auto* pv = st->find_pid(p)) list.push_back(ref_of(*pv));
    }
    session.remember(std::string(utterance), a.frame, subject, list, cfg_.history_limit, now);
    a.latency_ms = ms_since(t0);
    return a;
}

// ------------------------------------------------------------ rendering ---

std::string render_diagnosis(const reason::Diagnosis& d, bool verbose) {
    std::string o = d.headline + "\n";
    if (!d.evidence.empty()) {
        o += "\nEvidence (observed):\n";
        std::size_t shown = 0;
        for (const auto& e : d.evidence) {
            if (!verbose && shown >= 8) break;
            o += "  - " + e.text + "\n";
            ++shown;
        }
    }
    bool header = false;
    for (const auto& f : d.findings) {
        if (f.kind == reason::FindingKind::observation && !verbose && f.code.rfind("top_", 0) == 0) continue;
        if (!header) {
            o += "\nAnalysis:\n";
            header = true;
        }
        o += "  [" + kind_label(f.kind) + ", " + std::string(reason::to_string(f.certainty)) + "] " + f.text + "\n";
        for (const auto& m : f.mechanisms) {
            o += "      mechanism: " + m.from + " -" + m.relation + "-> " + m.to + " (" +
                 std::string(arch::to_string(m.nature));
            if (m.nature == arch::EdgeNature::hypothesized) o += ", p~" + fmt::num(static_cast<double>(m.confidence), 2);
            o += ")";
            if (verbose && !m.doc_ref.empty()) o += " [" + m.doc_ref + "]";
            o += "\n";
        }
    }
    if (!d.missing_evidence.empty()) {
        o += "\nNot observed / uncertain:\n";
        for (const auto& m : d.missing_evidence) o += "  - " + m + "\n";
    }
    o += "\nCertainty: " + std::string(reason::to_string(d.certainty));
    if (!d.next_step.empty()) o += ". Next: " + d.next_step;
    o += "\n";
    return o;
}

std::string Engine::render(Answer& a, const SystemState* s, const Resolution& res) {
    const auto& subject = res.subject;
    const auto& f = a.frame;
    const std::size_t n = f.top_n ? static_cast<std::size_t>(*f.top_n) : cfg_.default_top_n;
    std::string o;

    auto unavailable_note = [&]() {
        std::string u;
        for (const auto& x : f.unavailable_observations) u += "  - " + x + "\n";
        return u.empty() ? u : "\nUnavailable on this system:\n" + u;
    };

    // Action requests are refused (and audited) before anything else, so an
    // ambiguous or unresolvable target can never turn a refusal into a
    // "which process?" question that implies the action would then run.
    const std::string past_note =
        f.safety == intent::SafetyMode::past_event
            ? "OSIE keeps no record of past signals, kills or restarts, so it cannot say why or when that happened; "
              "it can only report current state.\n"
            : std::string();
    const std::string override_note =
        f.override_attempt && f.intent != Intent::action_request
            ? "Note: OSIE's read-only policy is fixed in its code; instructions or permission given in a conversation "
              "do not change it.\n"
            : std::string();
    if (f.ambiguous && !subject && f.intent != Intent::action_request) {
        return override_note + past_note + (f.clarification.empty() ? "Could you rephrase that?\n" : f.clarification + "\n");
    }
    if (!override_note.empty() || !past_note.empty()) {
        // Answer the read-only part of the question after the note.
        intent::IntentFrame plain = f;
        plain.override_attempt = false;
        plain.safety = intent::SafetyMode::none;
        Answer inner = a;
        inner.frame = plain;
        auto body = render(inner, s, res);
        a.diagnosis = inner.diagnosis;
        return override_note + past_note + body;
    }

    switch (f.intent) {
        case Intent::help:
            return "I answer questions about this machine's CPU, memory, processes, disks, network and power state, "
                   "using native OS interfaces (read-only). Try:\n"
                   "  what is using the most CPU?      why is my Mac slow?\n"
                   "  which app uses the most memory?  why is it using so much memory?\n"
                   "  how many cores do I have?        what is memory compression?\n"
                   "  which process started pid 123?   is memory pressure high?\n"
                   "Commands: :caps  :graph  :audit  :json  :verbose  :history  :quit\n";
        case Intent::unsupported:
            return "That is outside what OSIE understands (operating-system state and behaviour). Type 'help' for "
                   "examples.\n";
        case Intent::kubernetes:
            return "Kubernetes analysis is not implemented in this version. The design (typed resource graph, "
                   "pod->container->cgroup mapping, RBAC-scoped read access) is specified in docs/KUBERNETES.md.\n";
        case Intent::action_request: {
            const auto cap_id = intent::action_capability(f.action);
            const auto* cap = cap_id.empty() ? nullptr : reg_.find(cap_id);
            PolicyDecision d;
            if (cap != nullptr) {
                d = policy_.authorize(*cap, os_->platform(), os_->supported());
            } else {
                d = PolicyDecision{"act.<unmapped:" + std::string(intent::to_string(f.action)) + ">", false,
                                   "V1 safety policy is read-only: no state-changing operations exist",
                                   std::chrono::system_clock::now()};
            }
            audit_.record(d);
            static const std::map<intent::ActionType, std::string_view> kWhat = {
                {intent::ActionType::terminate, "terminate a process"},
                {intent::ActionType::set_priority, "change a process's priority"},
                {intent::ActionType::set_affinity, "change CPU affinity"},
                {intent::ActionType::kernel_parameter, "change a kernel parameter"},
                {intent::ActionType::cgroup_limit, "change a resource limit"},
                {intent::ActionType::restart_service, "restart, stop or start a service or the machine"},
                {intent::ActionType::network_config, "change network configuration"},
                {intent::ActionType::kubernetes_patch, "change Kubernetes resources"},
                {intent::ActionType::generic_change, "change the system"},
                {intent::ActionType::file_delete, "delete files"},
                {intent::ActionType::firewall_config, "change firewall rules"},
                {intent::ActionType::container_control, "stop, kill or restart a container"}};
            const auto what = kWhat.count(f.action) ? kWhat.at(f.action) : std::string_view("change the system");
            o = "I won't do that. This is a request to " + std::string(what) +
                ", and OSIE is read-only: it has no code that could carry this out (" + d.reason + ").\n";
            if (f.override_attempt || f.parser.find("+insistence") != std::string::npos)
                o += "Permission or instructions given in a conversation do not change this; the read-only policy is "
                     "fixed in OSIE's code.\n";
            if (cap != nullptr) {
                o += "  capability: " + cap->id + " (risk " + std::string(to_string(cap->risk)) + ", " +
                     (cap->reversible ? "reversible" : "irreversible") + "; requires " +
                     cap->required_permissions.to_string() + ")\n  side effects: " + cap->side_effects + "\n";
            }
            if (f.target.kind == TargetKind::process_name || f.target.kind == TargetKind::pid ||
                f.target.kind == TargetKind::reference) {
                std::string who = subject ? subject->name : f.target.value;
                if (!subject && f.target.kind == TargetKind::reference && sess_->last_process)
                    who = sess_->last_process->name;
                if (!subject && f.target.kind == TargetKind::pid) who = "pid " + f.target.value;
                o += "I can explain what the process is doing instead - ask 'why is " + who + " using so much CPU?'\n";
            }
            return o;
        }
        case Intent::explain_concept: {
            a.diagnosis = reasoner_.explain_concept(f.concept_key);
            o = a.diagnosis->headline + "\n";
            for (const auto& fi : a.diagnosis->findings) {
                o += "  - " + fi.text + " (" + std::string(arch::to_string(fi.mechanisms.front().nature)) + ")\n";
            }
            o += "Source: " + (a.diagnosis->missing_evidence.empty() ? std::string{} : a.diagnosis->missing_evidence.front()) + "\n";
            return o;
        }
        case Intent::what_if: {
            const std::string key = f.concept_key.empty() ? std::string(arch::concept_key::cpu_quota) : f.concept_key;
            a.diagnosis = reasoner_.explain_concept(key);
            o = "I can't give a numeric prediction here. OSIE answers what-if questions from its architecture model, "
                "and never presents a prediction as an observation.\n";
            if (f.change && f.change->resource == intent::HypotheticalChange::Resource::cpu_quota &&
                os_->platform() == Platform::linux_os) {
                const std::string who = f.target.kind == TargetKind::process_name ? "--name " + f.target.value
                                        : f.target.kind == TargetKind::pid      ? "--pid " + f.target.value
                                                                                : "--cgroup <path>";
                o += "An experimental CPU-quota predictor exists for Linux cgroup v2. It is validated only in the "
                     "environments listed in docs/WORLD_MODEL_RESULTS.md and must be enabled explicitly:\n"
                     "  osie predict --experimental --cpu-quota <cores> " + who + "\n";
            }
            o += "What the OS architecture model says about '" + a.diagnosis->headline + "':\n";
            for (const auto& fi : a.diagnosis->findings) o += "  - " + fi.text + "\n";
            if (os_->platform() == Platform::linux_os && s != nullptr) {
                for (const auto& cg : s->cgroups) {
                    if (!cg.info.cpu_quota_us) continue;
                    o += "  current: " + cg.info.path + " cpu.max " + std::to_string(*cg.info.cpu_quota_us) + "/" +
                         std::to_string(cg.info.cpu_period_us.value_or(100000)) + "\n";
                }
            }
            return o;
        }
        default: break;
    }
    if (s == nullptr) return "No observations were possible for this request.\n" + unavailable_note();

    switch (f.intent) {
        case Intent::top_cpu: {
            a.diagnosis = reasoner_.cpu(*s);
            auto top = s->top_by_cpu(n);
            if (top.empty()) return "Per-process CPU usage is unavailable.\n" + unavailable_note();
            o = "Processes using the most CPU (over the last " + fmt::num(std::chrono::duration<double>(s->interval).count(), 1) +
                " s; 100% = one full core):\n";
            for (std::size_t i = 0; i < top.size(); ++i) {
                const auto& p = *top[i];
                o += "  " + std::to_string(i + 1) + ". " + p.info.name + " (pid " + std::to_string(p.info.pid) + ")  " +
                     fmt::pct(*p.cpu_pct) + (p.info.threads ? "  " + std::to_string(*p.info.threads) + " threads" : "") + "\n";
            }
            a.diagnosis->ranked_pids.clear();
            for (const auto* p : top) a.diagnosis->ranked_pids.push_back(p->info.pid);
            if (s->cpu) o += "Machine total: " + fmt::pct(s->cpu->value.busy_pct()) + " busy across " +
                             std::to_string(s->logical_cpus()) + " logical CPUs.\n";
            if (s->processes_partial > 0)
                o += "Note: " + std::to_string(s->processes_partial) +
                     " processes of other users (root daemons) are not readable without privileges and are not ranked.\n";
            return o + unavailable_note();
        }
        case Intent::top_memory: {
            a.diagnosis = reasoner_.memory(*s);
            auto top = s->top_by_memory(n);
            if (top.empty()) return "Per-process memory is unavailable.\n" + unavailable_note();
            o = "Processes using the most memory (" + std::string(os_->platform() == Platform::macos ? "physical footprint" : "resident set") + "):\n";
            const double total = s->memory ? static_cast<double>(s->memory->value.total_bytes) : 0;
            for (std::size_t i = 0; i < top.size(); ++i) {
                const auto& p = *top[i];
                o += "  " + std::to_string(i + 1) + ". " + p.info.name + " (pid " + std::to_string(p.info.pid) + ")  " +
                     fmt::bytes(*p.info.memory_bytes());
                if (total > 0) o += "  (" + fmt::pct(100.0 * static_cast<double>(*p.info.memory_bytes()) / total) + " of RAM)";
                o += "\n";
            }
            a.diagnosis->ranked_pids.clear();
            for (const auto* p : top) a.diagnosis->ranked_pids.push_back(p->info.pid);
            if (s->memory && s->memory->value.pressure_level)
                o += "System memory pressure: " + std::string(to_string(*s->memory->value.pressure_level)) + ".\n";
            if (s->processes_partial > 0)
                o += "Note: " + std::to_string(s->processes_partial) + " processes of other users are not readable without privileges.\n";
            return o + unavailable_note();
        }
        case Intent::diagnose_cpu: a.diagnosis = reasoner_.cpu(*s); break;
        case Intent::diagnose_memory: a.diagnosis = reasoner_.memory(*s); break;
        case Intent::diagnose_slow: a.diagnosis = reasoner_.slowness(*s); break;
        case Intent::process_cpu:
            if (!subject) return "Which process do you mean?\n";
            a.diagnosis = reasoner_.process_cpu(*s, subject->pid);
            break;
        case Intent::process_memory:
            if (!subject) return "Which process do you mean?\n";
            a.diagnosis = reasoner_.process_memory(*s, subject->pid);
            break;
        case Intent::process_info:
        case Intent::process_tree: {
            if (!subject) {
                if (f.intent == Intent::process_tree && f.target.kind == TargetKind::host)
                    return "Which process? e.g. 'what started pid 123' or 'which service owns nginx'.\n";
                return "Which process do you mean?\n";
            }
            const auto* p = s->find_pid(subject->pid);
            if (p == nullptr) return "That process is no longer running.\n";
            const auto& pi = p->info;
            o = pi.name + " (pid " + std::to_string(pi.pid) + ")\n";
            if (!pi.path.empty()) o += "  executable: " + pi.path + "\n";
            o += "  state: " +
                 (os_->platform() == Platform::macos && pi.state == ProcessRunState::running
                      ? std::string("active (macOS reports process-level state only)")
                      : std::string(to_string(pi.state))) +
                 ", uid " + std::to_string(pi.uid) + "\n";
            if (p->cpu_pct) o += "  CPU: " + fmt::pct(*p->cpu_pct) + " of one core\n";
            if (auto m = pi.memory_bytes()) o += "  memory: " + fmt::bytes(*m) + "\n";
            if (pi.threads) o += "  threads: " + std::to_string(*pi.threads) + "\n";
            if (pi.validity == Validity::partial) o += "  (CPU/memory not readable: owned by another user)\n";
            // Ancestry from the architecture graph's PARENT_OF chain (observed edges).
            std::string chain;
            Pid cur = pi.ppid;
            for (int depth = 0; depth < 8 && cur > 0; ++depth) {
                const auto* pp = s->find_pid(cur);
                if (pp == nullptr) break;
                chain += (chain.empty() ? "" : " <- ") + pp->info.name + " (" + std::to_string(cur) + ")";
                if (pp->info.ppid == cur) break;
                cur = pp->info.ppid;
            }
            o += "  started by: " + (chain.empty() ? "pid " + std::to_string(pi.ppid) + " (not visible)" : chain) + "\n";
            const auto kids = s->children_of(pi.pid);
            if (!kids.empty()) {
                o += "  children: " + std::to_string(kids.size()) + " (";
                for (std::size_t i = 0; i < kids.size() && i < 5; ++i)
                    o += (i ? ", " : "") + kids[i]->info.name + " " + std::to_string(kids[i]->info.pid);
                o += kids.size() > 5 ? ", ...)\n" : ")\n";
            }
            if (os_->platform() == Platform::macos) {
                o += pi.pid == 1  ? "  this is launchd, the init process that manages all other services.\n"
                     : pi.ppid == 1 ? "  managed by: launchd (direct child). macOS exposes no public API mapping a PID to its "
                                    "launchd job label, so the exact service is not reported.\n"
                                  : "  managed by: not a direct launchd child; owned by its parent process above.\n";
            } else if (pi.cgroup) {
                o += "  cgroup: " + *pi.cgroup + "\n";
                if (auto unit = arch::systemd_unit_from_cgroup(*pi.cgroup))
                    o += "  managed by: " + *unit + " (inferred from the systemd cgroup layout)\n";
            }
            return o;
        }
        case Intent::system_info:
        case Intent::cpu_topology: {
            if (s->system) {
                const auto& si = s->system->value;
                o += si.os_name + " " + si.os_version + " (kernel " + si.kernel_release + ", " + si.arch + ")\n";
                if (!si.hardware_model.empty()) o += "  model: " + si.hardware_model + "\n";
                if (!si.cpu_brand.empty()) o += "  CPU: " + si.cpu_brand + "\n";
                o += "  memory: " + fmt::bytes(si.mem_total_bytes) + "\n";
                if (si.boot_time_unix) {
                    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    o += "  uptime: " + fmt::num(static_cast<double>(now - *si.boot_time_unix) / 3600.0, 1) + " h\n";
                }
                if (si.in_container) o += "  running inside a container\n";
            }
            if (s->topology) {
                const auto& t = s->topology->value;
                o += "  cores: " + std::to_string(t.physical_cores) + " physical, " + std::to_string(t.logical_cpus) +
                     " logical";
                if (t.clusters.size() > 1) {
                    o += " (";
                    for (std::size_t i = 0; i < t.clusters.size(); ++i)
                        o += (i ? " + " : "") + std::to_string(t.clusters[i].physical_cores) + " " + t.clusters[i].name;
                    o += ")";
                }
                o += "\n";
                if (t.l2_cache_bytes) o += "  L2 cache: " + fmt::bytes(*t.l2_cache_bytes) + "\n";
                if (t.l3_cache_bytes) o += "  L3 cache: " + fmt::bytes(*t.l3_cache_bytes) + "\n";
                if (!t.nominal_frequency_hz) o += "  clock frequency: not exposed by the OS\n";
            }
            return o + unavailable_note();
        }
        case Intent::memory_status: {
            if (!s->memory) return "Memory statistics are unavailable.\n" + unavailable_note();
            const auto& m = s->memory->value;
            o = "Memory: " + fmt::bytes(m.total_bytes) + " installed\n";
            if (m.available_bytes) o += "  available (estimate): " + fmt::bytes(*m.available_bytes) + "\n";
            if (m.free_bytes) o += "  free: " + fmt::bytes(*m.free_bytes) + "\n";
            if (m.wired_bytes) o += "  wired: " + fmt::bytes(*m.wired_bytes) + "\n";
            if (m.compressed_bytes) o += "  compressed: " + fmt::bytes(*m.compressed_bytes) + "\n";
            if (m.file_backed_bytes) o += "  file cache: " + fmt::bytes(*m.file_backed_bytes) + "\n";
            if (m.swap_used_bytes) o += "  swap used: " + fmt::bytes(*m.swap_used_bytes) + " of " + fmt::bytes(m.swap_total_bytes.value_or(0)) + "\n";
            if (m.pressure_level) o += "  pressure level: " + std::string(to_string(*m.pressure_level)) + "\n";
            return o + unavailable_note();
        }
        case Intent::disk_usage: {
            if (!s->filesystems) return "Filesystem information is unavailable.\n" + unavailable_note();
            o = "Filesystems:\n";
            for (const auto& fs : s->filesystems->value) {
                if (fs.total_bytes == 0 || fs.fs_type == "devfs" || fs.fs_type == "autofs" || fs.fs_type == "proc" ||
                    fs.fs_type == "sysfs" || fs.fs_type == "cgroup2" || fs.fs_type == "tmpfs" || fs.fs_type == "devpts" ||
                    fs.fs_type == "mqueue" || fs.fs_type == "nullfs")
                    continue;
                const double used = 100.0 * (1.0 - static_cast<double>(fs.available_bytes) / static_cast<double>(fs.total_bytes));
                o += "  " + fs.mount_point + " (" + fs.fs_type + ")  " + fmt::bytes(fs.available_bytes) + " free of " +
                     fmt::bytes(fs.total_bytes) + "  [" + fmt::pct(used, 0) + " used]" + (fs.read_only ? " ro" : "") + "\n";
            }
            if (s->filesystems->prov.note.size()) o += "Note: " + s->filesystems->prov.note + "\n";
            return o;
        }
        case Intent::network_status: {
            if (!s->net) return "Network information is unavailable.\n" + unavailable_note();
            o = "Network interfaces (up, with traffic):\n";
            for (const auto& ni : s->net->value) {
                if (!ni.up || ni.rx_bytes.value_or(0) + ni.tx_bytes.value_or(0) == 0) continue;
                o += "  " + ni.name + (ni.loopback ? " (loopback)" : "") + "  rx " + fmt::bytes(ni.rx_bytes.value_or(0)) +
                     " tx " + fmt::bytes(ni.tx_bytes.value_or(0));
                for (const auto& r : s->net_rates) {
                    if (r.name != ni.name) continue;
                    o += "  now " + fmt::bytes(static_cast<std::uint64_t>(r.rx_bytes_per_s.value_or(0))) + "/s in, " +
                         fmt::bytes(static_cast<std::uint64_t>(r.tx_bytes_per_s.value_or(0))) + "/s out";
                }
                if (!ni.addresses.empty()) o += "  " + ni.addresses.front();
                o += "\n";
            }
            o += "Per-process network attribution is not observed by OSIE V1.\n";
            return o;
        }
        case Intent::thermal_status: {
            if (s->power) {
                o = "Thermal state: " + std::string(to_string(s->power->value.thermal)) + " (macOS qualitative signal; " +
                    "temperatures in degrees are not exposed by a public API).\n";
                o += std::string("Low Power Mode: ") + (s->power->value.low_power_mode ? "on" : "off") + "\n";
                if (s->cpu) o += "CPU currently " + fmt::pct(s->cpu->value.busy_pct()) + " busy.\n";
                return o;
            }
            return "Thermal state is not available on this platform.\n" + unavailable_note();
        }
        case Intent::list_processes: {
            if (!s->processes) return "The process list is unavailable.\n" + unavailable_note();
            const auto& ps = s->processes->value;
            std::vector<const ProcessView*> v;
            v.reserve(ps.size());
            for (const auto& p : ps) v.push_back(&p);
            std::stable_sort(v.begin(), v.end(), [](const ProcessView* x, const ProcessView* y) {
                return x->cpu_pct.value_or(-1) > y->cpu_pct.value_or(-1);
            });
            std::map<std::string, std::size_t> states;
            for (const auto* p : v) states[std::string(to_string(p->info.state))]++;
            o = std::to_string(ps.size()) + " processes exist";
            if (os_->platform() == Platform::linux_os) {
                // Linux reports a per-task scheduler state; XNU's process-level
                // status is SRUN for almost every live process, so it is not shown.
                o += " (";
                bool first = true;
                for (const auto& [st, c] : states) {
                    o += (first ? "" : ", ") + std::to_string(c) + " " + st;
                    first = false;
                }
                o += ")";
            }
            o += ".\n";
            const std::size_t show = f.top_n ? n : std::min<std::size_t>(15, v.size());
            o += "Listed by CPU over the last " + fmt::num(std::chrono::duration<double>(s->interval).count(), 1) +
                 " s (100% = one core):\n";
            for (std::size_t i = 0; i < show && i < v.size(); ++i) {
                const auto& p = *v[i];
                o += "  " + std::to_string(p.info.pid) + "  " + p.info.name;
                if (p.cpu_pct) o += "  " + fmt::pct(*p.cpu_pct);
                if (auto m = p.info.memory_bytes()) o += "  " + fmt::bytes(*m);
                o += "\n";
            }
            if (v.size() > show) o += "  ... and " + std::to_string(v.size() - show) + " more (use --json for all).\n";
            a.diagnosis = reason::Diagnosis{};
            a.diagnosis->topic = "processes";
            for (std::size_t i = 0; i < show && i < v.size(); ++i) a.diagnosis->ranked_pids.push_back(v[i]->info.pid);
            if (s->processes_partial > 0)
                o += "Note: CPU and memory of " + std::to_string(s->processes_partial) +
                     " processes owned by other users are not readable without privileges.\n";
            if (s->processes_vanished > 0)
                o += "Note: " + std::to_string(s->processes_vanished) + " processes exited while the list was being read.\n";
            return o + unavailable_note();
        }
        case Intent::cpu_capacity: return render_capacity(f, subject) + unavailable_note();
        case Intent::cgroup_status: {
            if (os_->platform() != Platform::linux_os)
                return "cgroups are a Linux mechanism. On macOS, containers run inside a Linux VM whose cgroups are not "
                       "visible to a macOS process; run OSIE inside that VM or container to inspect them.\n";
            if (s->cgroups.empty()) return "No cgroup could be inspected.\n" + unavailable_note();
            o = "cgroups of the " + std::to_string(cfg_.cgroup_scan_n) + " busiest processes (" +
                std::to_string(s->cgroups.size()) + " distinct), rates over the last " +
                fmt::num(std::chrono::duration<double>(cfg_.sample_interval).count(), 1) + " s:\n";
            std::size_t constrained = 0;
            for (const auto& cg : s->cgroups) {
                const auto& ci = cg.info;
                std::string line = "  " + ci.path + "\n     CPU quota ";
                if (ci.cpu_quota_us)
                    line += fmt::num(static_cast<double>(*ci.cpu_quota_us) /
                                         static_cast<double>(std::max<std::uint64_t>(1, ci.cpu_period_us.value_or(100000))), 2) +
                            " CPUs";
                else line += ci.nr_periods ? "none (max)" : "unknown (cpu controller not enabled)";
                if (ci.cpuset_cpus) line += "; cpuset " + std::to_string(*ci.cpuset_cpus) + " CPUs";
                if (cg.window_usage_cpus) line += "; using " + fmt::num(*cg.window_usage_cpus, 2) + " CPUs";
                bool hit = false;
                if (cg.window_throttled_ratio) {
                    line += "; throttled in " + fmt::pct(100.0 * *cg.window_throttled_ratio) + " of periods";
                    hit = *cg.window_throttled_ratio >= 0.1;
                } else if (cg.lifetime_throttled_ratio) {
                    line += "; throttled in " + fmt::pct(100.0 * *cg.lifetime_throttled_ratio) + " of periods (lifetime)";
                }
                if (ci.memory_max) {
                    line += "; memory " + fmt::bytes(ci.memory_current.value_or(0)) + " of " + fmt::bytes(*ci.memory_max);
                    if (ci.memory_current && *ci.memory_current * 10 >= *ci.memory_max * 9) hit = true;
                }
                for (const auto& ps : ci.pressure)
                    if (ps.resource == PsiResource::cpu) line += "; PSI cpu some " + fmt::pct(ps.some.avg10, 1);
                if (!cg.window_note.empty()) line += " (" + cg.window_note + ")";
                if (hit) {
                    ++constrained;
                    line += "\n     -> constrained";
                }
                o += line + "\n";
            }
            o += constrained == 0 ? "None of these cgroups is currently constrained by its CPU quota or memory limit.\n"
                                  : std::to_string(constrained) + " cgroup(s) currently constrained (>=10% of periods "
                                    "throttled, or memory within 10% of memory.max).\n";
            o += "Only cgroups of the busiest processes are inspected; idle cgroups are not listed.\n";
            return o + unavailable_note();
        }
        case Intent::container_throttle: {
            if (os_->platform() != Platform::linux_os) {
                return "Container CPU throttling is a Linux cgroup mechanism. On macOS, Docker/Podman containers run "
                       "inside a Linux VM, whose cgroups are not visible to a macOS process; run OSIE inside that VM "
                       "or container to inspect them.\n";
            }
            a.diagnosis = reasoner_.cpu(*s);
            break;
        }
        default: break;
    }
    if (a.diagnosis) {
        reason::annotate_evidence(*a.diagnosis, *s);
        o = render_diagnosis(*a.diagnosis, false);
        if (!f.unavailable_observations.empty()) o += unavailable_note();
        if (res.group.size() > 1 && subject && (f.intent == Intent::process_cpu || f.intent == Intent::process_memory))
            o = render_group(*s, res, f) + o;
    }
    return o;
}

std::string Engine::render_capacity(const intent::IntentFrame& f, const std::optional<ProcessRef>& subject) {
    const Pid pid = subject ? subject->pid : 0;
    auto r = broker_.cpu_capacity(pid);
    if (!r) return "CPU capacity could not be determined: " + r.error().describe() + "\n";
    const auto& c = r->value;
    std::string o = "CPU capacity for " + (subject ? subject->name + " (pid " + std::to_string(subject->pid) + ")" : c.scope) + ":\n";
    auto line = [&](std::string_view name, const CpuLimit& l, std::string_view unit) {
        std::string x = "  " + std::string(name) + ": ";
        switch (l.status) {
            case LimitStatus::known: x += fmt::num(*l.cpus, unit == "CPUs of time" ? 2 : 0) + " " + std::string(unit); break;
            case LimitStatus::unlimited: x += "no limit"; break;
            case LimitStatus::unknown: x += "unknown"; break;
            case LimitStatus::not_applicable: x += "not applicable"; break;
        }
        if (!l.source.empty() && l.status != LimitStatus::not_applicable) x += "  [" + l.source + "]";
        if (!l.note.empty()) x += "  (" + l.note + ")";
        return x + "\n";
    };
    o += line("host logical CPUs", c.host_logical, "CPUs");
    o += line("online CPUs", c.online, "CPUs");
    o += line("CPU affinity (where it may run)", c.affinity, "CPUs");
    o += line("cgroup cpuset", c.cpuset, "CPUs");
    o += line("cgroup CPU quota (how much time)", c.quota, "CPUs of time");
    for (const auto& q : c.quota_chain) o += "      cpu.max " + q + "\n";
    if (c.effective_cpus) {
        o += "Effective capacity: at most " + fmt::num(*c.effective_cpus, 2) + " CPUs, set by the " + c.binding + ".\n";
        if (c.quota.status == LimitStatus::known && c.affinity.status == LimitStatus::known && c.quota.cpus &&
            c.affinity.cpus && *c.quota.cpus < *c.affinity.cpus)
            o += "The quota limits CPU *time*, not placement: threads can run on any of the " +
                 fmt::num(*c.affinity.cpus, 0) + " allowed CPUs, but together get at most " + fmt::num(*c.quota.cpus, 2) +
                 " CPUs' worth of time per period, after which they are throttled.\n";
    } else {
        o += "Effective capacity: unknown (no CPU count could be measured).\n";
    }
    if (!c.unknown_limits.empty()) {
        o += "Could not be measured, so the true limit may be lower: ";
        for (std::size_t i = 0; i < c.unknown_limits.size(); ++i) o += (i ? ", " : "") + c.unknown_limits[i];
        o += ".\n";
    }
    (void)f;
    return o;
}

namespace {

std::string clock_time(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return buf;
}

std::string duration_text(double s) {
    if (s < 90) return fmt::num(s, 0) + " s";
    if (s < 5400) return fmt::num(s / 60.0, 1) + " min";
    return fmt::num(s / 3600.0, 1) + " h";
}

struct SeriesSpec {
    std::string label;
    std::string unit;      // appended to values
    TrendRule rule;
    double scale{1.0};     // display = value * scale
};

std::string fmt_value(double v, const SeriesSpec& sp) { return fmt::num(v * sp.scale, sp.unit == "%" ? 0 : 1) + sp.unit; }

std::string describe_trend(const SeriesSpec& sp, const TrendResult& r) {
    if (r.direction == TrendDirection::insufficient_data) return sp.label + ": not enough history (" + r.why + ").";
    std::string o = sp.label + ": ";
    switch (r.direction) {
        case TrendDirection::increasing: o += "increasing"; break;
        case TrendDirection::decreasing: o += "decreasing"; break;
        default: o += "no clear change"; break;
    }
    o += " over " + duration_text(r.span_s) + " (" + std::to_string(r.n) + " samples): " + fmt_value(r.first, sp) + " -> " +
         fmt_value(r.last, sp) + "; first-third mean " + fmt_value(r.early_mean, sp) + ", last-third mean " +
         fmt_value(r.late_mean, sp) + ", range " + fmt_value(r.min, sp) + "-" + fmt_value(r.max, sp) +
         " (Mann-Kendall z=" + fmt::num(r.mann_kendall_z, 1) + ").";
    return o;
}

}  // namespace

std::string Engine::render_temporal(Answer& a, const Resolution& res) {
    using intent::TemporalKind;
    using intent::TemporalMetric;
    const auto& q = *a.frame.temporal;
    const StateHistory& history = ev_->history();
    const auto& all = history.points();
    const double span_min = history.config().min_span_s;
    const auto now = std::chrono::steady_clock::now();
    a.history_evaluated_at = now;
    a.history_evaluated_wall = std::chrono::system_clock::now();
    std::vector<const HistoryPoint*> pts;
    for (const auto& p : all)
        if (!q.window_s || now - p.at <= std::chrono::seconds(*q.window_s)) pts.push_back(&p);
    // Each analysed series, with where its points came from (for --json and the API).
    using Origin = Answer::SeriesOrigin;
    auto add_trend = [&a](std::string label, const TrendResult& t, Origin origin) {
        a.trends.emplace_back(std::move(label), t);
        a.trend_origins.push_back(std::move(origin));
    };
    auto note = [](Origin& org, const HistoryPoint& p) {
        if (!org.first) {
            org.first = p.wall;
            org.first_seq = p.seq;
        }
        org.last = p.wall;
        org.last_seq = p.seq;
    };

    std::string o;
    const double recorded_s =
        all.empty() ? 0.0 : std::chrono::duration<double>(all.back().at - all.front().at).count();
    if (q.window_s && recorded_s + 1.0 < static_cast<double>(*q.window_s)) {
        o += "Note: history in this session covers only " + duration_text(recorded_s) + " of the requested " +
             duration_text(static_cast<double>(*q.window_s)) + "; the answer is limited to that.\n";
    }
    auto t_of = [&](const HistoryPoint& p) { return std::chrono::duration<double>(p.at - pts.front()->at).count(); };
    auto how_to_record = [&]() {
        return std::string("OSIE only knows what it has sampled in this session (") + std::to_string(all.size()) +
               " sample(s) so far) and keeps no history on disk. Start a session with 'osie --watch 5' to record every "
               "5 s, or use 'osie ask --watch 2 --for 60 \"...\"', then ask again.\n";
    };

    // Series builders over the windowed points.
    auto host_series = [&](TemporalMetric m, Origin* org = nullptr) {
        Series s;
        for (const auto* p : pts) {
            const std::size_t before = s.size();
            if (m == TemporalMetric::cpu && p->cpu_busy_pct) s.emplace_back(t_of(*p), *p->cpu_busy_pct);
            if (m == TemporalMetric::memory && p->mem_available_bytes && p->mem_total_bytes && *p->mem_total_bytes > 0)
                s.emplace_back(t_of(*p), 100.0 * (1.0 - static_cast<double>(*p->mem_available_bytes) /
                                                            static_cast<double>(*p->mem_total_bytes)));
            if (org != nullptr && s.size() != before) note(*org, *p);
        }
        return s;
    };
    // A point belongs to the process only if its identity matches: (pid,
    // start time), or, where a start time is missing on either side, the
    // pid and the name. A reused PID is never stitched into the series.
    auto same_process = [](const ProcessRef& r, const ProcessPoint& pp) {
        if (pp.pid != r.pid) return false;
        if (r.start_time_us && pp.start_time_us) return *r.start_time_us == *pp.start_time_us;
        return pp.name == r.name;
    };
    auto proc_series = [&](const ProcessRef& r, bool mem, Origin* org = nullptr) {
        Series s;
        for (const auto* p : pts)
            for (const auto& pp : p->processes)
                if (same_process(r, pp)) {
                    const std::size_t before = s.size();
                    if (mem && pp.memory_bytes) s.emplace_back(t_of(*p), static_cast<double>(*pp.memory_bytes) / (1 << 20));
                    if (!mem && pp.cpu_pct) s.emplace_back(t_of(*p), *pp.cpu_pct);
                    if (org != nullptr && s.size() != before) note(*org, *p);
                }
        return s;
    };
    SeriesSpec cpu_spec{"CPU busy (whole machine)", "%", TrendRule{4, span_min, 5.0, 1.96}};
    SeriesSpec mem_spec{"Memory in use (whole machine)", "%", TrendRule{4, span_min, 2.0, 1.96}};

    if (pts.empty()) return o + "I have no observations for that period. " + how_to_record();

    // ---- onset: when did memory pressure (or high CPU) begin? -------------
    if (q.kind == TemporalKind::onset) {
        const bool pressure = q.metric == TemporalMetric::memory_pressure || q.metric == TemporalMetric::memory;
        auto elevated = [&](const HistoryPoint& p) -> std::optional<bool> {
            if (pressure) {
                if (p.pressure) return *p.pressure != MemoryPressureLevel::normal;
                if (p.psi_mem_some10) return *p.psi_mem_some10 >= 10.0;
                return std::nullopt;
            }
            if (p.cpu_busy_pct) return *p.cpu_busy_pct >= 75.0;
            return std::nullopt;
        };
        const std::string what = pressure ? (os_->platform() == Platform::macos
                                                 ? "Memory pressure (kernel level warning or critical)"
                                                 : "Memory pressure (PSI memory some avg10 >= 10%)")
                                          : "High CPU (>= 75% busy)";
        std::optional<std::size_t> start;
        std::size_t known = 0;
        bool currently = false;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            auto e = elevated(*pts[i]);
            if (!e) continue;
            ++known;
            if (*e && !currently) start = i;
            currently = *e;
        }
        if (known == 0) return o + what + " was not observable in the recorded samples.\n";
        const auto ago = [&](std::size_t i) {
            return clock_time(pts[i]->wall) + " (" +
                   duration_text(std::chrono::duration<double>(now - pts[i]->at).count()) + " ago)";
        };
        if (!currently) {
            if (start) return o + what + " is not present now; the most recent episode began at " + ago(*start) + ".\n";
            return o + what + " was not observed in " + std::to_string(known) + " samples since " + ago(0) + ".\n";
        }
        if (start && *start == 0)
            return o + what + " was already present when recording began at " + ago(0) +
                   ", so when it started is unknown.\n" + (all.size() < 4 ? how_to_record() : std::string());
        return o + what + " began at " + ago(*start) + " and is still present (" + std::to_string(known) +
               " samples).\n";
    }

    // ---- which process has been growing? ----------------------------------
    if (q.rank_processes) {
        const bool mem = a.frame.intent == Intent::top_memory;
        // Keyed by identity: (pid, start time), and the name where the start
        // time is unknown, so that a reused PID is a separate process.
        std::map<std::tuple<Pid, std::uint64_t, std::string>, ProcessRef> ids;
        for (const auto* p : pts)
            for (const auto& pp : p->processes)
                ids.emplace(std::make_tuple(pp.pid, pp.start_time_us.value_or(0), pp.start_time_us ? std::string() : pp.name),
                            ProcessRef{pp.pid, pp.start_time_us, pp.name});
        struct Row {
            ProcessRef r;
            TrendResult t;
            Origin origin;
        };
        std::vector<Row> rows;
        for (const auto& [_, r] : ids) {
            TrendRule rule = mem ? TrendRule{4, span_min, 16.0, 1.96} : TrendRule{4, span_min, 5.0, 1.96};
            Origin org;
            org.process = r;
            auto t = analyse_trend(proc_series(r, mem, &org), rule);
            if (t.direction == TrendDirection::increasing) rows.push_back({r, t, std::move(org)});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
            return x.t.late_mean - x.t.early_mean > y.t.late_mean - y.t.early_mean;
        });
        if (pts.size() < 4 || t_of(*pts.back()) < span_min) {
            TrendResult none;
            none.n = pts.size();
            none.span_s = t_of(*pts.back());
            none.why = std::to_string(pts.size()) + " samples over " + duration_text(none.span_s);
            Origin org;
            note(org, *pts.front());
            note(org, *pts.back());
            add_trend(std::string("processes: ") + (mem ? "memory" : "CPU"), none, std::move(org));
            return o + "Not enough history to tell which processes are growing (" + std::to_string(pts.size()) +
                   " samples over " + duration_text(t_of(*pts.back())) + "). " + how_to_record();
        }
        o += std::string("Processes whose ") + (mem ? "memory" : "CPU use") + " increased over " +
             duration_text(t_of(*pts.back())) + " (" + std::to_string(pts.size()) + " samples):\n";
        if (rows.empty()) o += "  none showed a clear increase.\n";
        for (std::size_t i = 0; i < rows.size() && i < 5; ++i) {
            const auto& [r, t, origin] = rows[i];
            // The series behind each ranked line, for --json and the API.
            add_trend(r.name + " (pid " + std::to_string(r.pid) + ") " + (mem ? "memory (MiB)" : "CPU (%)"), t, origin);
            o += "  " + std::to_string(i + 1) + ". " + r.name + " (pid " + std::to_string(r.pid) + ")  " +
                 (mem ? fmt::num(t.early_mean, 0) + " -> " + fmt::num(t.late_mean, 0) + " MiB"
                      : fmt::pct(t.early_mean) + " -> " + fmt::pct(t.late_mean)) +
                 "  (" + std::to_string(t.n) + " samples)\n";
        }
        o += "Only the top " + std::to_string(history.config().top_k) +
             " processes by CPU and by memory are kept per sample, so smaller processes are not covered.\n";
        return o;
    }

    // ---- trend / recovery for one process or the whole machine -------------
    std::vector<std::pair<SeriesSpec, Series>> series;
    std::vector<Origin> origins;
    if (res.subject) {
        const bool mem = a.frame.intent == Intent::process_memory;
        const auto& r = *res.subject;
        SeriesSpec sp{r.name + " (pid " + std::to_string(r.pid) + ") " + (mem ? "memory" : "CPU"), mem ? " MiB" : "%",
                      mem ? TrendRule{4, span_min, 16.0, 1.96} : TrendRule{4, span_min, 5.0, 1.96}};
        Origin org;
        org.process = r;
        series.emplace_back(sp, proc_series(r, mem, &org));
        origins.push_back(std::move(org));
    } else {
        if (q.metric != TemporalMetric::memory && q.metric != TemporalMetric::memory_pressure) {
            origins.emplace_back();
            series.emplace_back(cpu_spec, host_series(TemporalMetric::cpu, &origins.back()));
        }
        if (q.metric != TemporalMetric::cpu) {
            origins.emplace_back();
            series.emplace_back(mem_spec, host_series(TemporalMetric::memory, &origins.back()));
        }
    }
    bool insufficient = false;
    for (std::size_t si = 0; si < series.size(); ++si) {
        auto& [sp, s] = series[si];
        auto t = analyse_trend(s, sp.rule);
        add_trend(sp.label, t, origins[si]);
        insufficient |= t.direction == TrendDirection::insufficient_data;
        if (q.kind == TemporalKind::recovery && t.direction != TrendDirection::insufficient_data) {
            // Recovering = the latest values are clearly below the peak and
            // the series after the peak is decreasing (or already flat and low).
            std::size_t peak = 0;
            for (std::size_t i = 1; i < s.size(); ++i)
                if (s[i].second > s[peak].second) peak = i;
            Series after(s.begin() + static_cast<std::ptrdiff_t>(peak), s.end());
            const auto ta = analyse_trend(after, TrendRule{3, span_min / 2, sp.rule.min_effect, sp.rule.z_threshold});
            const double drop = s[peak].second - s.back().second;
            std::string verdict;
            if (peak + 1 >= s.size() || drop < sp.rule.min_effect) verdict = "not recovering: it is at or near its peak";
            else if (ta.direction == TrendDirection::decreasing || drop >= 2 * sp.rule.min_effect)
                verdict = "recovering: down " + fmt_value(drop, sp) + " from its peak";
            else verdict = "unclear: slightly below its peak, without a clear downward trend";
            o += sp.label + ": " + verdict + " (peak " + fmt_value(s[peak].second, sp) + ", now " +
                 fmt_value(s.back().second, sp) + ", " + std::to_string(s.size()) + " samples over " +
                 duration_text(t.span_s) + ").\n";
        } else {
            o += describe_trend(sp, t) + "\n";
        }
    }
    if (res.subject && !series.empty() && series.front().second.size() < pts.size())
        o += "(The process was in the tracked set for " + std::to_string(series.front().second.size()) + " of " +
             std::to_string(pts.size()) + " samples; missing samples are not counted as zero.)\n";
    if (insufficient) o += how_to_record();
    else o += "Method: Mann-Kendall trend test plus a minimum effect size; consecutive samples are correlated, so treat "
              "this as descriptive, not as a causal explanation.\n";
    return o;
}

std::string Engine::render_group(const SystemState& s, const Resolution& res, const intent::IntentFrame& f) const {
    if (!res.subject) return {};
    const bool cpu = f.intent == Intent::process_cpu;
    double cpu_sum = 0;
    std::uint64_t mem_sum = 0;
    std::size_t cpu_known = 0, mem_known = 0;
    std::string rows;
    for (std::size_t i = 0; i < res.group.size(); ++i) {
        const auto* p = s.find_pid(res.group[i].pid);
        if (p == nullptr) continue;
        if (p->cpu_pct) cpu_sum += *p->cpu_pct, ++cpu_known;
        if (auto m = p->info.memory_bytes()) mem_sum += *m, ++mem_known;
        if (i < 5) {
            rows += "  " + std::to_string(i + 1) + ". " + p->info.name + " (pid " + std::to_string(p->info.pid) + ")  " +
                    (cpu ? (p->cpu_pct ? fmt::pct(*p->cpu_pct) : std::string("CPU n/a"))
                         : (p->info.memory_bytes() ? fmt::bytes(*p->info.memory_bytes()) : std::string("memory n/a"))) +
                    "\n";
        }
    }
    if (res.group.size() > 5) rows += "  ... and " + std::to_string(res.group.size() - 5) + " more\n";
    std::string o = std::to_string(res.group.size()) + " processes match '" + f.target.value + "'";
    if (res.partial_matches > 0)
        o += " (another " + std::to_string(res.partial_matches) + " contain it in their name and are not included)";
    o += ".\n";
    if (cpu && cpu_known > 0) o += "Together: " + fmt::pct(cpu_sum) + " of one core (" + std::to_string(cpu_known) + " readable).\n";
    if (!cpu && mem_known > 0)
        o += "Together: " + fmt::bytes(mem_sum) + " (" + std::to_string(mem_known) +
             " readable; shared memory may be counted more than once).\n";
    o += rows + "The analysis below is for the " + (cpu ? "busiest" : "largest") + " instance, pid " +
         std::to_string(res.subject->pid) + ". Ask about another one by PID or e.g. 'the second one'.\n\n";
    return o;
}

}  // namespace osie

// ------------------------------------------------------------ prediction ---

namespace osie {

namespace {

std::string fmt_val(double v, const std::string& unit) {
    if (unit == "fraction") return fmt::pct(100.0 * v);
    return fmt::num(v, 2) + " " + unit;
}

std::string target_label(const std::string& name) {
    if (name == "usage_cores") return "CPU usage";
    if (name == "throttled_cores") return "Throttled CPU time (runnable but held back by the quota)";
    if (name == "throttled_ratio") return "Share of quota periods throttled";
    if (name == "psi_some") return "CPU pressure (PSI some: share of time a task waited)";
    return name;
}

}  // namespace

PredictionAnswer Engine::predict(const PredictTarget& target, predict::ActionKind kind, double proposed,
                                 const CancellationToken& cancel) {
    std::lock_guard lk(mu_);
    PredictionAnswer ans;
    const auto* cap = reg_.find("observe.predict.cpu_world_model");
    if (cap == nullptr) {
        ans.text = "Prediction is not available in this build.\n";
        return ans;
    }
    if (!cfg_.experimental_prediction) {
        ans.text = "CPU predictions are experimental and off by default: they were validated only in the environments "
                   "listed in docs/WORLD_MODEL_RESULTS.md, and some workload classes are known to be predicted badly. "
                   "Enable them explicitly with --experimental (or OSIE_EXPERIMENTAL_PREDICT=1). Nothing was "
                   "observed or predicted.\n";
        return ans;
    }
    const auto decision = policy_.authorize(*cap, os_->platform(), os_->supported());
    audit_.record(decision);
    if (!decision.allowed) {
        ans.text = os_->platform() == Platform::macos
                       ? "CPU quota prediction is defined for Linux cgroup v2 CPU bandwidth control, which macOS does not "
                         "have. Nothing was predicted.\n"
                       : "Prediction is not available here: " + decision.reason + ".\n";
        return ans;
    }
    if (!world_model_) {
        ans.text = "No CPU world model is installed (models/world_cpu.osiewm), so OSIE cannot predict this. "
                   "Nothing was predicted.\n";
        return ans;
    }
    if (!world_model_->supports(kind)) {
        ans.text = "The installed world model was not trained for '" + std::string(predict::to_string(kind)) +
                   "' interventions. Nothing was predicted.\n";
        return ans;
    }

    // ---- resolve the cgroup ---------------------------------------------
    std::string path = target.cgroup;
    std::string who = path;
    if (path.empty()) {
        SampleRequest req;
        req.kinds = {ObservationKind::process_list};
        req.interval = Millis{50};
        const auto st = sampler_.sample(req, cancel);
        intent::IntentFrame f;
        f.intent = Intent::process_info;
        if (target.pid) f.target = intent::Target{TargetKind::pid, std::to_string(*target.pid), std::nullopt};
        else f.target = intent::Target{TargetKind::process_name, target.process_name, std::nullopt};
        const auto res = session_.resolve(f, st, std::chrono::steady_clock::now(), cfg_.context_ttl);
        if (!res.subject) {
            ans.text = (f.clarification.empty() ? "Which process do you mean?" : f.clarification) + "\n";
            return ans;
        }
        auto cg = broker_.cgroup_of(res.subject->pid);
        if (!cg) {
            ans.text = "Cannot read the cgroup of " + res.subject->name + " (pid " + std::to_string(res.subject->pid) +
                       "): " + cg.error().describe() + "\n";
            return ans;
        }
        path = *cg;
        who = res.subject->name + " (pid " + std::to_string(res.subject->pid) + "), cgroup " + path;
    }
    ans.cgroup = path;

    // ---- observe the model's history window ------------------------------
    const auto win = std::chrono::duration<double>(world_model_->window_seconds());
    std::vector<predict::CgroupWindow> hist;
    auto snap = [&]() -> std::optional<std::pair<CgroupInfo, std::optional<CpuSample>>> {
        auto c = broker_.cgroup(path);
        if (!c) return std::nullopt;
        std::optional<CpuSample> h;
        if (auto s = broker_.cpu_sample()) h = s->value;
        return std::make_pair(c->value, h);
    };
    auto prev = snap();
    if (!prev) {
        auto c = broker_.cgroup(path);
        ans.text = "Cannot observe cgroup " + path + ": " + (c ? std::string("unknown error") : c.error().describe()) + "\n";
        return ans;
    }
    auto t_prev = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < world_model_->history_windows() && !cancel.cancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(win));
        auto cur = snap();
        const auto t_cur = std::chrono::steady_clock::now();
        if (!cur) break;
        const double secs = std::chrono::duration<double>(t_cur - t_prev).count();
        auto w = predict::make_window(prev->first, cur->first, secs, prev->second ? &*prev->second : nullptr,
                                      cur->second ? &*cur->second : nullptr);
        if (w) hist.push_back(*w);
        prev = cur;
        t_prev = t_cur;
    }
    if (hist.empty()) {
        ans.text = "Could not collect any valid observation window for " + path + ". Nothing was predicted.\n";
        return ans;
    }

    // ---- intervention and model ------------------------------------------
    predict::Intervention a;
    a.kind = kind;
    a.proposed = proposed;
    const auto& last = hist.back();
    const double cpus = last.cpuset_cpus.value_or(last.host_cpus.value_or(1.0));
    if (kind == predict::ActionKind::cpu_quota) a.previous = last.quota_cores.value_or(cpus);
    if (kind == predict::ActionKind::cpuset) a.previous = cpus;
    predict::RuntimeEnvironment env;
    if (auto si = broker_.system_info()) {
        env.arch = si->value.arch;
        env.kernel_release = si->value.kernel_release;
    }
    env.cgroup_version = os_->supported().contains(ObservationKind::cgroup) ? 2 : 1;
    auto p = world_model_->predict_in(hist, a, env);
    if (!p) {
        ans.text = "Cannot evaluate this intervention: " + p.error().message + ". Nothing was predicted.\n";
        return ans;
    }
    ans.prediction = *p;

    // ---- render ------------------------------------------------------------
    std::string o = "OSIE prediction (hypothetical: nothing will be changed)\n";
    o += "Target: " + who + "\n";
    const std::string unit = kind == predict::ActionKind::cpu_quota ? " cores" : " CPUs";
    const std::string what = kind == predict::ActionKind::cpu_quota ? "CPU quota" : "cpuset size";
    o += what + ": " + (kind == predict::ActionKind::cpu_quota && !last.quota_cores ? "unlimited (" + fmt::num(cpus, 0) + " CPUs usable)"
                                                                                       : fmt::num(a.previous, 2) + unit) +
         " -> " + fmt::num(a.proposed, 2) + unit + "\n";
    o += "Observed over " + fmt::num(static_cast<double>(hist.size()) * world_model_->window_seconds(), 1) + " s (" +
         std::to_string(hist.size()) + " windows).\n\n";
    if (p->abstained) {
        o += "Prediction unavailable.\nReason:\n";
        for (const auto& r : p->reasons) o += "  - " + r + "\n";
        o += "OSIE falls back to observation: ask 'why is this container throttled?' for the current state.\n";
    } else {
        for (const auto& t : p->targets) {
            o += target_label(t.name) + (t.status == "experimental" ? " (experimental: interval coverage not established)" : "") + ":\n";
            o += "  now " + fmt_val(t.current, t.unit) + "  ->  expected " + fmt_val(t.expected, t.unit);
            for (const auto& iv : t.intervals)
                o += "  [" + fmt::pct(100.0 * iv.level, 0) + " interval " + fmt_val(iv.lo, t.unit) + " - " + fmt_val(iv.hi, t.unit) + "]";
            if (t.clamped) o += "  (raw model output was outside physical bounds and was clamped)";
            o += "\n";
        }
        if (p->throttling_assessment == "low")
            o += "Throttling: predicted low (even the upper interval bound is small). This is not a guarantee: the "
                 "validation false-safe rate is reported in docs/WORLD_MODEL_RESULTS.md.\n";
        else if (p->throttling_assessment == "likely")
            o += "Throttling: likely (even the lower interval bound is material).\n";
        else if (p->throttling_assessment == "possible")
            o += "Throttling: cannot be ruled out (the interval includes material throttling).\n";
        ans.answered = true;
    }
    o += "\nNot predicted: application latency and throughput. OSIE cannot observe them for this workload, and the "
         "model's latency results come only from instrumented lab workloads.\n";
    o += "Assumptions:\n";
    o += "  - the workload's demand stays as observed during the window; a change in offered load invalidates the prediction\n";
    // Mechanisms from the architecture graph, labelled with their epistemic
    // status. The graph explains the prediction; it does not produce it.
    const auto start_key = kind == predict::ActionKind::cpu_quota ? arch::concept_key::cpu_quota
                                                                  : arch::concept_key::cpu_throttling;
    if (const auto* n = graph_.find(arch::NodeKind::concept_, start_key)) {
        for (auto eid : graph_.consequences(n->id, Platform::linux_os, 2)) {
            const auto& ed = graph_.edge(eid);
            o += "  - " + graph_.node(ed.from).label + " -" + std::string(arch::to_string(ed.kind)) + "-> " +
                 graph_.node(ed.to).label + " (" + std::string(arch::to_string(ed.nature)) +
                 (ed.doc_ref.empty() ? "" : "; " + ed.doc_ref) + ")\n";
        }
    }
    o += "  - validated only in: " +
         (world_model_->provenance().empty() ? std::string("unknown") : world_model_->provenance()) + "\n";
    o += "Model: " + world_model_->id() + ". Evidence: cgroup " + path + " cpu.stat, cpu.max, cpu.pressure, "
         "cpuset.cpus.effective.\n";
    o += "No changes were applied.\n";
    ans.text = o;
    return ans;
}

}  // namespace osie
