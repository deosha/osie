// SPDX-License-Identifier: Apache-2.0
#include "osie/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <set>

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

bool is_process_intent(Intent i) {
    return i == Intent::process_cpu || i == Intent::process_memory || i == Intent::process_info ||
           i == Intent::process_tree;
}

ProcessRef ref_of(const ProcessView& p) { return ProcessRef{p.info.pid, p.info.start_time_us, p.info.name}; }

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

std::optional<ProcessRef> SessionContext::resolve(intent::IntentFrame& f, const SystemState& s) const {
    auto fail = [&f](std::string q) -> std::optional<ProcessRef> {
        f.ambiguous = true;
        f.clarification = std::move(q);
        return std::nullopt;
    };
    switch (f.target.kind) {
        case TargetKind::pid: {
            const Pid pid = static_cast<Pid>(std::stol(f.target.value));
            if (const auto* p = s.find_pid(pid)) return ref_of(*p);
            return fail("No running process has PID " + f.target.value + " (it may have exited).");
        }
        case TargetKind::process_name: {
            auto matches = s.find_by_name(f.target.value);
            if (matches.empty()) return fail("I can't find a running process matching '" + f.target.value + "'.");
            // Pick the most relevant instance for the question.
            const ProcessView* best = matches.front();
            if (f.intent == Intent::process_cpu) {
                for (const auto* m : matches)
                    if (m->cpu_pct.value_or(-1) > best->cpu_pct.value_or(-1)) best = m;
            }
            return ref_of(*best);
        }
        case TargetKind::reference: {
            const ProcessRef* r = nullptr;
            if (f.target.ordinal && !last_list.empty()) {
                const int o = *f.target.ordinal;
                if (o == -1) r = &last_list.back();
                else if (o >= 1 && static_cast<std::size_t>(o) <= last_list.size()) r = &last_list[static_cast<std::size_t>(o - 1)];
            } else if (last_process) {
                r = &*last_process;
            } else if (!last_list.empty()) {
                r = &last_list.front();
            }
            if (r == nullptr) return fail("Which process do you mean? I have no earlier process to refer to.");
            const auto* p = s.find_pid(r->pid);
            if (p == nullptr || (r->start_time_us && p->info.start_time_us && *r->start_time_us != *p->info.start_time_us)) {
                return fail(r->name + " (pid " + std::to_string(r->pid) + ") is no longer running.");
            }
            return ref_of(*p);
        }
        case TargetKind::host:
        case TargetKind::none: break;
    }
    return std::nullopt;
}

void SessionContext::remember(const std::string& utterance, const intent::IntentFrame& f,
                              const std::optional<ProcessRef>& subject, const std::vector<ProcessRef>& list,
                              std::size_t limit) {
    last_intent = f.intent;
    if (subject) last_process = subject;
    if (!list.empty()) {
        last_list = list;
        if (!subject) last_process = list.front();
    }
    history.emplace_back(utterance, f.intent);
    while (history.size() > limit) history.pop_front();
}

// -------------------------------------------------------------- engine ---

Engine::Engine(std::unique_ptr<IOperatingSystem> os, std::unique_ptr<intent::IIntentParser> parser, EngineConfig cfg)
    : os_(std::move(os)),
      parser_(std::move(parser)),
      cfg_(cfg),
      reg_(CapabilityRegistry::builtin()),
      broker_(*os_, reg_, policy_, audit_),
      sampler_(broker_),
      reasoner_(graph_, os_->platform()) {
    arch::load_ontology(graph_);
}

Engine::~Engine() = default;

intent::IntentFrame Engine::interpret(std::string_view utterance) const {
    const auto toks = intent::tokenize(utterance);
    auto out = parser_->parse(toks);
    auto f = intent::build_frame(out, toks, os_->platform(), os_->supported(), reg_);
    f.validation_errors = intent::validate(f);
    return f;
}

std::shared_ptr<const SystemState> Engine::state_for(const std::vector<ObservationKind>& kinds,
                                                     const CancellationToken& c, bool& reused) {
    reused = false;
    if (kinds.empty()) return nullptr;
    if (auto s = cache_.fresh(kinds, cfg_.freshness_budget)) {
        reused = true;
        return s;
    }
    SampleRequest req;
    req.kinds = kinds;
    req.interval = cfg_.sample_interval;
    // Always include system info + topology: cheap and needed for context.
    for (auto k : {ObservationKind::system_info, ObservationKind::cpu_topology})
        if (std::find(req.kinds.begin(), req.kinds.end(), k) == req.kinds.end()) req.kinds.push_back(k);
    auto st = std::make_shared<SystemState>(sampler_.sample(req, c));
    arch::update_instances(graph_, *st, os_->platform());
    cache_.store(st);
    return st;
}

std::shared_ptr<const SystemState> Engine::observe_all(const CancellationToken& c) {
    std::vector<ObservationKind> all;
    for (auto k : os_->supported().list()) all.push_back(k);
    bool reused = false;
    return state_for(all, c, reused);
}

Answer Engine::ask(std::string_view utterance, const CancellationToken& cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    Answer a;
    const auto toks = intent::tokenize(utterance);
    auto out = parser_->parse(toks);
    a.frame = intent::build_frame(out, toks, os_->platform(), os_->supported(), reg_);
    a.parse_ms = ms_since(t0);

    // Context: short follow-ups and references.
    if (session_.apply_followup(a.frame, toks)) {
        a.frame = intent::build_frame(intent::ParseOutput{a.frame.intent, a.frame.confidence, {}, a.frame.parser + "+context"},
                                      toks, os_->platform(), os_->supported(), reg_);
        if (is_process_intent(a.frame.intent)) {
            a.frame.target = intent::Target{TargetKind::reference, "previous process", std::nullopt};
            a.frame.ambiguous = false;
            a.frame.clarification.clear();
        }
    }

    bool reused = false;
    std::shared_ptr<const SystemState> st;
    if (!a.frame.ambiguous || is_process_intent(a.frame.intent)) {
        st = state_for(a.frame.observations, cancel, reused);
    }
    std::optional<ProcessRef> subject;
    if (st && a.frame.target.kind != TargetKind::host && a.frame.target.kind != TargetKind::none) {
        subject = session_.resolve(a.frame, *st);
        if (subject) {
            a.frame.ambiguous = false;
            a.frame.clarification.clear();
        }
    }
    a.frame.validation_errors = intent::validate(a.frame);
    a.reused_observations = reused;
    if (st) a.observation_age = std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - st->taken_at);

    a.text = render(a, st.get(), subject);

    std::vector<ProcessRef> list;
    if (a.diagnosis && st) {
        for (Pid p : a.diagnosis->ranked_pids)
            if (const auto* pv = st->find_pid(p)) list.push_back(ref_of(*pv));
    }
    session_.remember(std::string(utterance), a.frame, subject, list, cfg_.history_limit);
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

std::string Engine::render(Answer& a, const SystemState* s, const std::optional<ProcessRef>& subject) {
    const auto& f = a.frame;
    const std::size_t n = f.top_n ? static_cast<std::size_t>(*f.top_n) : cfg_.default_top_n;
    std::string o;

    auto unavailable_note = [&]() {
        std::string u;
        for (const auto& x : f.unavailable_observations) u += "  - " + x + "\n";
        return u.empty() ? u : "\nUnavailable on this system:\n" + u;
    };

    if (f.ambiguous && !subject) {
        return f.clarification.empty() ? "Could you rephrase that?\n" : f.clarification + "\n";
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
            o = "I won't do that: " + d.reason + ".\n";
            if (cap != nullptr) {
                o += "  capability: " + cap->id + " (risk " + std::string(to_string(cap->risk)) + ", " +
                     (cap->reversible ? "reversible" : "irreversible") + "; requires " +
                     cap->required_permissions.to_string() + ")\n  side effects: " + cap->side_effects + "\n";
            }
            if (f.target.kind == TargetKind::process_name || f.target.kind == TargetKind::pid ||
                f.target.kind == TargetKind::reference) {
                std::string who = subject ? subject->name : f.target.value;
                if (!subject && f.target.kind == TargetKind::reference && session_.last_process)
                    who = session_.last_process->name;
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
            o = "OSIE V1 cannot predict the numeric effect of that change: it has no validated predictive model for "
                "it, and predictions are never presented as observations.\nWhat the OS architecture model says about '" +
                a.diagnosis->headline + "':\n";
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
            o += "  state: " + std::string(to_string(pi.state)) + ", uid " + std::to_string(pi.uid) + "\n";
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
                o += pi.ppid == 1 ? "  managed by: launchd (direct child). macOS exposes no public API mapping a PID to its "
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
        o = render_diagnosis(*a.diagnosis, false);
        if (!f.unavailable_observations.empty()) o += unavailable_note();
    }
    return o;
}

}  // namespace osie
