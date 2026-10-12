// SPDX-License-Identifier: Apache-2.0
// Tokenizer, lexicons, deterministic frame construction and schema validation.
#include <algorithm>
#include <cctype>
#include <set>

#include "osie/intent/intent.hpp"
#include "osie/util/json.hpp"

namespace osie::intent {

namespace {
constexpr std::array<std::string_view, kIntentCount> kIntentNames = {
#define OSIE_X(n) #n,
    OSIE_INTENTS(OSIE_X)
#undef OSIE_X
};

bool tok_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-' || c >= 0x80;
}
bool strip_char(char c) { return c == '.' || c == '_' || c == '-'; }

bool has(const std::vector<Token>& t, std::string_view w) {
    return std::any_of(t.begin(), t.end(), [&](const Token& x) { return x.lower == w; });
}
}  // namespace

std::string_view to_string(Intent i) noexcept {
    const auto idx = static_cast<std::size_t>(i);
    return idx < kIntentNames.size() ? kIntentNames[idx] : "unsupported";
}

std::optional<Intent> intent_from_string(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kIntentNames.size(); ++i)
        if (kIntentNames[i] == s) return static_cast<Intent>(i);
    return std::nullopt;
}

std::string_view to_string(SpanType t) noexcept {
    switch (t) {
        case SpanType::proc: return "PROC";
        case SpanType::pid: return "PID";
        case SpanType::ref: return "REF";
        case SpanType::num: return "NUM";
    }
    return "?";
}

std::string_view to_string(TargetKind k) noexcept {
    switch (k) {
        case TargetKind::host: return "host";
        case TargetKind::process_name: return "process_name";
        case TargetKind::pid: return "pid";
        case TargetKind::reference: return "reference";
        case TargetKind::none: return "none";
    }
    return "?";
}

std::string_view to_string(ActionType a) noexcept {
    switch (a) {
        case ActionType::none: return "none";
        case ActionType::terminate: return "terminate";
        case ActionType::set_priority: return "set_priority";
        case ActionType::set_affinity: return "set_affinity";
        case ActionType::kernel_parameter: return "kernel_parameter";
        case ActionType::cgroup_limit: return "cgroup_limit";
        case ActionType::restart_service: return "restart_service";
        case ActionType::network_config: return "network_config";
        case ActionType::kubernetes_patch: return "kubernetes_patch";
        case ActionType::generic_change: return "generic_change";
    }
    return "?";
}

std::string_view action_capability(ActionType a) noexcept {
    switch (a) {
        case ActionType::terminate: return "act.process.terminate";
        case ActionType::set_priority: return "act.process.set_priority";
        case ActionType::set_affinity: return "act.process.set_affinity";
        case ActionType::kernel_parameter: return "act.kernel.set_parameter";
        case ActionType::cgroup_limit: return "act.cgroup.set_cpu_limit";
        case ActionType::restart_service: return "act.service.restart";
        case ActionType::network_config: return "act.network.configure";
        case ActionType::kubernetes_patch: return "act.kubernetes.patch";
        case ActionType::generic_change:
        case ActionType::none: break;
    }
    return "";
}

std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        while (i < n && !tok_char(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t b = i;
        while (i < n && tok_char(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t e = i;
        while (b < e && strip_char(text[b])) ++b;
        while (e > b && strip_char(text[e - 1])) --e;
        if (b == e) continue;
        Token t;
        t.text = std::string(text.substr(b, e - b));
        t.lower = t.text;
        for (auto& c : t.lower)
            if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        t.begin = b;
        t.end = e;
        out.push_back(std::move(t));
    }
    return out;
}

std::optional<int> parse_ordinal(std::string_view w) {
    static constexpr std::pair<std::string_view, int> table[] = {
        {"first", 1}, {"top", 1}, {"1st", 1}, {"second", 2}, {"2nd", 2}, {"third", 3}, {"3rd", 3},
        {"fourth", 4}, {"4th", 4}, {"fifth", 5}, {"5th", 5}, {"last", -1}};
    for (const auto& [k, v] : table)
        if (k == w) return v;
    return std::nullopt;
}

std::optional<std::string> find_concept(const std::vector<Token>& toks) {
    // Longest phrases first. Values are ontology concept keys (architecture/model.hpp).
    static const std::vector<std::pair<std::vector<std::string_view>, std::string_view>> lex = {
        {{"run", "queue"}, "run_queue"},
        {{"runqueue"}, "run_queue"},
        {{"memory", "compression"}, "memory_compression"},
        {{"compressed", "memory"}, "memory_compression"},
        {{"compressor"}, "memory_compression"},
        {{"memory", "pressure"}, "memory_pressure"},
        {{"page", "cache"}, "page_cache"},
        {{"file", "cache"}, "page_cache"},
        {{"page", "faults"}, "major_page_faults"},
        {{"page", "fault"}, "major_page_faults"},
        {{"virtual", "memory"}, "virtual_memory"},
        {{"load", "average"}, "load_average"},
        {{"cpu", "limit"}, "cgroup_cpu_quota"},
        {{"cpu", "quota"}, "cgroup_cpu_quota"},
        {{"cpu.max"}, "cgroup_cpu_quota"},
        {{"memory", "limit"}, "cgroup_memory_limit"},
        {{"efficiency", "cores"}, "heterogeneous_cores"},
        {{"performance", "cores"}, "heterogeneous_cores"},
        {{"e-cores"}, "heterogeneous_cores"},
        {{"p-cores"}, "heterogeneous_cores"},
        {{"quality", "of", "service"}, "qos_classes"},
        {{"qos"}, "qos_classes"},
        {{"steal", "time"}, "hypervisor_steal"},
        {{"oom"}, "out_of_memory_kill"},
        {{"jetsam"}, "out_of_memory_kill"},
        {{"psi"}, "psi_cpu"},
        {{"pressure", "stall"}, "psi_cpu"},
        {{"throttling"}, "cpu_bandwidth_throttling"},
        {{"throttled"}, "cpu_bandwidth_throttling"},
        {{"thermal"}, "thermal_pressure"},
        {{"scheduler"}, "scheduler"},
        {{"scheduling"}, "scheduler"},
        {{"swapping"}, "swapping"},
        {{"swap"}, "swapping"},
        {{"paging"}, "paging"},
        {{"latency"}, "scheduling_latency"},
    };
    std::size_t best_len = 0;
    std::optional<std::string> best;
    for (const auto& [phrase, key] : lex) {
        if (phrase.size() <= best_len) continue;
        for (std::size_t i = 0; i + phrase.size() <= toks.size(); ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < phrase.size() && ok; ++j) ok = toks[i + j].lower == phrase[j];
            if (ok) {
                best_len = phrase.size();
                best = std::string(key);
                break;
            }
        }
    }
    return best;
}

ActionType find_action(const std::vector<Token>& toks) {
    auto any = [&](std::initializer_list<std::string_view> ws) {
        return std::any_of(ws.begin(), ws.end(), [&](std::string_view w) { return has(toks, w); });
    };
    if (any({"kubernetes", "k8s", "pod", "pods", "deployment", "kubectl", "replicas"})) return ActionType::kubernetes_patch;
    if (any({"kill", "terminate", "quit", "pkill", "killall", "end", "force-quit", "close", "shut"})) return ActionType::terminate;
    if (any({"renice", "nice", "priority", "prioritize", "deprioritize", "niceness"})) return ActionType::set_priority;
    if (any({"affinity", "pin", "taskset"})) return ActionType::set_affinity;
    if (any({"sysctl", "tunable", "kernel", "swappiness", "parameter"})) return ActionType::kernel_parameter;
    if (any({"cgroup", "quota", "limit", "cpu.max", "limits"})) return ActionType::cgroup_limit;
    if (any({"restart", "reboot", "reload", "relaunch", "service", "daemon"})) return ActionType::restart_service;
    if (any({"dns", "ip", "route", "wifi", "firewall", "interface", "network"})) return ActionType::network_config;
    return ActionType::generic_change;
}

namespace {

std::vector<ObservationKind> required_for(Intent i) {
    using K = ObservationKind;
    switch (i) {
        case Intent::top_cpu: return {K::cpu_times, K::cpu_topology, K::process_list};
        case Intent::top_memory: return {K::memory, K::process_list};
        case Intent::diagnose_cpu:
            return {K::cpu_times, K::cpu_topology, K::load_average, K::process_list, K::thermal_state,
                    K::pressure_stall, K::cgroup};
        case Intent::diagnose_memory:
            return {K::memory, K::swap, K::memory_pressure_level, K::process_list, K::pressure_stall};
        case Intent::diagnose_slow:
            return {K::cpu_times, K::cpu_topology, K::load_average, K::process_list, K::memory, K::swap,
                    K::memory_pressure_level, K::thermal_state, K::pressure_stall, K::filesystems, K::cgroup};
        case Intent::process_cpu: return {K::cpu_times, K::cpu_topology, K::process_list, K::cgroup};
        case Intent::process_memory: return {K::memory, K::process_list};
        case Intent::process_info: return {K::cpu_times, K::process_list};
        case Intent::process_tree: return {K::process_list, K::cgroup};
        case Intent::system_info: return {K::system_info, K::cpu_topology, K::memory};
        case Intent::cpu_topology: return {K::cpu_topology, K::system_info};
        case Intent::memory_status: return {K::memory, K::swap, K::memory_pressure_level};
        case Intent::disk_usage: return {K::filesystems};
        case Intent::network_status: return {K::network_interfaces};
        case Intent::thermal_status: return {K::thermal_state, K::cpu_times};
        case Intent::container_throttle: return {K::cgroup, K::pressure_stall, K::process_list, K::cpu_times};
        case Intent::explain_concept:
        case Intent::what_if:
        case Intent::action_request:
        case Intent::kubernetes:
        case Intent::help:
        case Intent::unsupported:
        case Intent::count_: break;
    }
    return {};
}

std::vector<std::string> entities_for(Intent i, const std::vector<Token>& toks) {
    std::set<std::string> e;
    switch (i) {
        case Intent::top_cpu: case Intent::diagnose_cpu: case Intent::process_cpu: e.insert("cpu"); break;
        case Intent::top_memory: case Intent::diagnose_memory: case Intent::process_memory: case Intent::memory_status:
            e.insert("memory"); break;
        case Intent::disk_usage: e.insert("disk"); break;
        case Intent::network_status: e.insert("network"); break;
        case Intent::thermal_status: e.insert("thermal"); break;
        case Intent::container_throttle: e.insert("cgroup"); e.insert("cpu"); break;
        case Intent::diagnose_slow: e.insert("cpu"); e.insert("memory"); e.insert("disk"); break;
        default: break;
    }
    if (i == Intent::top_cpu || i == Intent::top_memory || i == Intent::diagnose_cpu || i == Intent::diagnose_slow ||
        i == Intent::process_cpu || i == Intent::process_memory || i == Intent::process_info || i == Intent::process_tree) {
        e.insert("processes");
    }
    for (const auto& t : toks) {
        if (t.lower == "swap" || t.lower == "swapping") e.insert("swap");
        if (t.lower == "scheduler" || t.lower == "scheduling") e.insert("scheduler");
        if (t.lower == "service" || t.lower == "services" || t.lower == "daemon") e.insert("service");
        if (t.lower == "container" || t.lower == "docker") e.insert("container");
    }
    return {e.begin(), e.end()};
}

std::optional<int> word_number(std::string_view w) {
    static constexpr std::pair<std::string_view, int> words[] = {
        {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6},
        {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}, {"twenty", 20}};
    for (const auto& [k, v] : words)
        if (k == w) return v;
    if (!w.empty() && w.size() <= 4 && std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        return std::stoi(std::string(w));
    return std::nullopt;
}

bool is_process_intent(Intent i) {
    return i == Intent::process_cpu || i == Intent::process_memory || i == Intent::process_info ||
           i == Intent::process_tree;
}

}  // namespace

IntentFrame build_frame(const ParseOutput& p, const std::vector<Token>& toks, Platform platform,
                        const ObservationSet& supported, const CapabilityRegistry& reg) {
    IntentFrame f;
    f.intent = p.intent;
    f.confidence = p.confidence;
    f.parser = p.parser;
    f.target.kind = TargetKind::host;

    // Target: PID > named process > reference.
    const Span* pid = nullptr;
    const Span* proc = nullptr;
    const Span* ref = nullptr;
    for (const auto& s : p.spans) {
        if (s.begin >= s.end || s.end > toks.size()) continue;
        if (s.type == SpanType::pid && !pid) pid = &s;
        if (s.type == SpanType::proc && !proc) proc = &s;
        if (s.type == SpanType::ref && !ref) ref = &s;
        if (s.type == SpanType::num && !f.top_n && (p.intent == Intent::top_cpu || p.intent == Intent::top_memory))
            f.top_n = word_number(toks[s.begin].lower);
    }
    auto span_text = [&](const Span& s) {
        std::string v;
        for (std::size_t i = s.begin; i < s.end; ++i) {
            if (i > s.begin) v += ' ';
            v += toks[i].text;
        }
        return v;
    };
    if (pid) {
        std::string digits;
        for (std::size_t i = pid->begin; i < pid->end; ++i)
            for (char c : toks[i].text)
                if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        if (!digits.empty() && digits.size() <= 9) {
            f.target = Target{TargetKind::pid, digits, std::nullopt};
        }
    }
    if (f.target.kind == TargetKind::host && proc) f.target = Target{TargetKind::process_name, span_text(*proc), std::nullopt};
    if (f.target.kind == TargetKind::host && ref) {
        f.target = Target{TargetKind::reference, span_text(*ref), std::nullopt};
        for (std::size_t i = ref->begin; i < ref->end; ++i)
            if (auto o = parse_ordinal(toks[i].lower)) f.target.ordinal = o;
    }
    // Host-level intents always target the host; stray names/references in
    // e.g. "why is it so slow" must not turn them into process questions.
    const bool targets_process = is_process_intent(f.intent) || f.intent == Intent::action_request ||
                                 f.intent == Intent::container_throttle;
    if (!targets_process) f.target = Target{TargetKind::host, {}, std::nullopt};
    if (is_process_intent(f.intent) && f.target.kind == TargetKind::host) {
        f.target.kind = TargetKind::none;
        f.ambiguous = true;
        f.clarification = "Which process do you mean? Give a name or PID.";
    }
    if (f.intent == Intent::help || f.intent == Intent::unsupported || f.intent == Intent::explain_concept ||
        f.intent == Intent::kubernetes) {
        if (f.target.kind == TargetKind::host) f.target.kind = TargetKind::none;
    }

    f.entities = entities_for(f.intent, toks);
    if (f.intent == Intent::explain_concept || f.intent == Intent::what_if) {
        if (auto c = find_concept(toks)) f.concept_key = *c;
        if (f.intent == Intent::explain_concept && f.concept_key.empty()) {
            f.ambiguous = true;
            f.clarification = "Which OS concept should I explain (e.g. scheduler, memory compression, run queue)?";
        }
    }
    if (f.intent == Intent::action_request) f.action = find_action(toks);
    if (f.intent == Intent::kubernetes) {
        f.unsupported_reason = "Kubernetes analysis is specified in docs/KUBERNETES.md but not implemented in V1";
    }
    if (f.intent == Intent::unsupported) {
        f.unsupported_reason = "request is outside OSIE's operating-system scope";
    }
    if ((f.intent == Intent::top_cpu || f.intent == Intent::top_memory) && !f.top_n) {
        for (std::size_t i = 0; i + 1 < toks.size(); ++i)
            if (toks[i].lower == "top")
                if (auto n = word_number(toks[i + 1].lower)) f.top_n = n;
        if (!f.top_n && (has(toks, "most") || has(toks, "biggest") || has(toks, "highest") || has(toks, "heaviest")) &&
            !has(toks, "processes") && !has(toks, "apps") && !has(toks, "applications")) {
            f.top_n = 1;
        }
    }

    for (auto k : required_for(f.intent)) {
        if (supported.contains(k)) {
            f.observations.push_back(k);
            if (const auto* cap = reg.for_observation(k)) {
                if (std::find(f.capabilities.begin(), f.capabilities.end(), cap->id) == f.capabilities.end())
                    f.capabilities.push_back(cap->id);
            }
        } else {
            const auto* cap = reg.for_observation(k);
            const bool platform_lacks = cap != nullptr && !cap->platforms.contains(platform);
            f.unavailable_observations.push_back(std::string(to_string(k)) +
                                                 (platform_lacks ? " (not available on " + std::string(to_string(platform)) + ")"
                                                                 : " (not supported by this kernel/adapter)"));
        }
    }
    if (f.intent == Intent::action_request) {
        const auto cap = action_capability(f.action);
        if (!cap.empty()) f.capabilities.emplace_back(cap);
    }
    return f;
}

std::vector<std::string> validate(const IntentFrame& f) {
    std::vector<std::string> v;
    if (static_cast<std::size_t>(f.intent) >= kIntentCount) v.emplace_back("intent out of range");
    if (!f.read_only) v.emplace_back("V1 frames must be read_only");
    if (f.confidence < 0.0F || f.confidence > 1.0F) v.emplace_back("confidence outside [0,1]");
    if (f.top_n && (*f.top_n < 1 || *f.top_n > 50)) v.emplace_back("top_n must be within 1..50");
    if (is_process_intent(f.intent)) {
        if (f.target.kind == TargetKind::host || f.target.kind == TargetKind::none) {
            if (!f.ambiguous) v.emplace_back("process intent requires a process target");
        }
    }
    if (f.target.kind == TargetKind::pid) {
        if (f.target.value.empty() ||
            !std::all_of(f.target.value.begin(), f.target.value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            v.emplace_back("pid target must be numeric");
    }
    if (f.target.kind == TargetKind::process_name && f.target.value.empty()) v.emplace_back("empty process name");
    if (f.intent == Intent::action_request) {
        if (f.action == ActionType::none) v.emplace_back("action_request without action type");
    } else if (f.action != ActionType::none) {
        v.emplace_back("action set on non-action intent");
    }
    for (const auto& c : f.capabilities) {
        if (f.intent != Intent::action_request && c.rfind("observe.", 0) != 0)
            v.emplace_back("non-observe capability on read intent: " + c);
    }
    return v;
}

std::string to_json(const IntentFrame& f) {
    json::Writer w;
    w.begin_object();
    w.kv("intent", to_string(f.intent)).kv("confidence", static_cast<double>(f.confidence)).kv("parser", f.parser);
    w.key("target").begin_object().kv("kind", to_string(f.target.kind));
    if (!f.target.value.empty()) w.kv("value", f.target.value);
    if (f.target.ordinal) w.kv("ordinal", *f.target.ordinal);
    w.end_object();
    w.key("entities").begin_array();
    for (const auto& e : f.entities) w.value(e);
    w.end_array();
    w.key("constraints").begin_object().kv("read_only", f.read_only);
    if (f.top_n) w.kv("top_n", *f.top_n);
    w.end_object();
    if (f.action != ActionType::none) w.kv("action", to_string(f.action));
    if (!f.concept_key.empty()) w.kv("concept", f.concept_key);
    w.key("observations").begin_array();
    for (auto k : f.observations) w.value(to_string(k));
    w.end_array();
    w.key("unavailable_observations").begin_array();
    for (const auto& u : f.unavailable_observations) w.value(u);
    w.end_array();
    w.key("capabilities").begin_array();
    for (const auto& c : f.capabilities) w.value(c);
    w.end_array();
    w.kv("ambiguous", f.ambiguous);
    if (!f.clarification.empty()) w.kv("clarification", f.clarification);
    if (!f.unsupported_reason.empty()) w.kv("unsupported_reason", f.unsupported_reason);
    w.key("validation_errors").begin_array();
    for (const auto& e : f.validation_errors) w.value(e);
    w.end_array();
    w.end_object();
    return w.take();
}

}  // namespace osie::intent
