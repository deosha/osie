// SPDX-License-Identifier: Apache-2.0
// Token normalisation and safety-intent detection.
//
// Safety detection decides whether an utterance *asks OSIE to change
// something* before any parser runs. Keywords alone cannot do this ("how does
// kill -9 work?" and "kill -9 4242" share their keywords), so the utterance is
// split into clauses and each clause is classified by its grammatical role:
// an imperative or a request addressed to OSIE is a directive; the same verb
// inside a question, an if-clause, a past-tense report or quotes is not.
// The result only ever *adds* a refusal or removes a spurious one; it never
// grants a capability (there is no action executor to grant).
#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

#include "osie/intent/intent.hpp"

namespace osie::intent {
namespace {

using WordSet = std::unordered_set<std::string_view>;

// ------------------------------------------------------------ normalise ---

const std::unordered_map<std::string_view, std::string_view>& abbreviations() {
    static const std::unordered_map<std::string_view, std::string_view> m = {
        {"procs", "processes"}, {"proc", "process"},   {"pls", "please"},    {"plz", "please"},
        {"u", "you"},           {"ur", "your"},        {"whats", "what"},    {"wats", "what"},
        {"thats", "that"},      {"hows", "how"},       {"im", "i"},          {"dont", "do"},
        {"cant", "can"},        {"util", "utilization"}, {"perf", "performance"}, {"sys", "system"},
        {"teh", "the"},         {"hte", "the"},        {"waht", "what"},     {"whta", "what"},
        {"wat", "what"},        {"mem", "memory"},     {"cpus", "cpus"},     {"gimme", "give"},
        {"svc", "service"},     {"cg", "cgroup"},
    };
    return m;
}

// Vocabulary that typo correction may map *to*. Only OSIE cue words: a token
// is never "corrected" towards an arbitrary English word or a process name.
const std::vector<std::string_view>& cue_vocabulary() {
    static const std::vector<std::string_view> v = {
        "process", "processes", "running", "program", "programs", "application", "applications", "memory",
        "using", "consuming", "throttled", "throttling", "pressure", "cgroup", "cgroups", "container",
        "containers", "terminate", "restart", "temperature", "network", "kernel", "version", "uptime", "usage",
        "available", "increased", "increasing", "swapping", "sluggish", "bottleneck", "quota", "limit", "limits",
        "running", "currently", "processor", "utilization", "consumption", "constrained", "capacity", "effective",
        "machine", "computer", "system", "server", "slowing", "which", "what", "show", "list", "most", "highest",
        "biggest", "growing", "recovering", "explain", "difference", "affinity", "priority", "firewall",
        "delete", "remove", "reboot", "predict", "happen", "happens", "would", "health", "healthy", "thermal",
        "overheating", "storage", "swap", "cores", "threads", "memory", "running", "apps",
    };
    return v;
}

bool alpha(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
}

// Optimal-string-alignment distance, bounded: returns limit+1 when exceeded.
std::size_t osa_distance(std::string_view a, std::string_view b, std::size_t limit) {
    const std::size_t n = a.size(), m = b.size();
    if ((n > m ? n - m : m - n) > limit) return limit + 1;
    std::vector<std::size_t> d((n + 1) * (m + 1));
    auto at = [&](std::size_t i, std::size_t j) -> std::size_t& { return d[i * (m + 1) + j]; };
    for (std::size_t i = 0; i <= n; ++i) at(i, 0) = i;
    for (std::size_t j = 0; j <= m; ++j) at(0, j) = j;
    for (std::size_t i = 1; i <= n; ++i) {
        std::size_t row_min = limit + 1;
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            std::size_t v = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1, at(i - 1, j - 1) + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, at(i - 2, j - 2) + 1);
            at(i, j) = v;
            row_min = std::min(row_min, v);
        }
        if (row_min > limit) return limit + 1;
    }
    return at(n, m);
}

// Words that must never be "corrected" although they are near a cue word.
const WordSet kKeep = {"show", "shows", "most", "what", "which", "using", "used", "user", "users", "uses", "list",
                       "lost", "last", "past", "post", "host", "hosts", "core", "more", "mode", "node", "nodes",
                       "free", "tree", "limits", "limit", "slow", "slack", "stack", "steam", "server", "service",
                       "services", "serve", "swift", "swap", "sway", "lima", "mail", "make", "made", "take",
                       "sleep", "stop", "step", "kill", "fill", "nice", "dice", "apps", "apis", "maps", "pods",
                       "loop", "load", "read", "ready", "reads", "redis", "ruby", "rust", "docker", "chrome",
                       "chromium", "python", "java", "node", "nginx", "mysql", "postgres", "kernel", "kernels",
                       "the", "they", "them", "then", "than", "that", "this", "with", "have", "having", "shell",
                       "shall", "still", "system", "systemd", "terminal", "programs", "progress", "proxy"};

}  // namespace

void normalize(std::vector<Token>& toks) {
    const auto& ab = abbreviations();
    const auto& vocab = cue_vocabulary();
    for (auto& t : toks) {
        if (auto it = ab.find(t.lower); it != ab.end()) {
            t.lower = std::string(it->second);
            continue;
        }
        if (t.lower.size() < 5 || !alpha(t.lower) || kKeep.count(t.lower)) continue;
        if (std::find(vocab.begin(), vocab.end(), t.lower) != vocab.end()) continue;
        const std::size_t limit = t.lower.size() >= 8 ? 2 : 1;
        std::string_view best;
        std::size_t best_d = limit + 1;
        bool tie = false;
        for (auto w : vocab) {
            if (w.size() < 4) continue;
            const auto d = osa_distance(t.lower, w, limit);
            if (d < best_d) {
                best_d = d;
                best = w;
                tie = false;
            } else if (d == best_d && w != best) {
                tie = true;
            }
        }
        // First letters must agree: a capitalised name such as "Pages" or
        // "Teams" is far more likely a process than a misspelt cue word.
        if (best_d <= limit && !tie && best.front() == t.lower.front()) t.lower = std::string(best);
    }
}

std::string_view to_string(SafetyMode m) noexcept {
    switch (m) {
        case SafetyMode::none: return "none";
        case SafetyMode::directive: return "directive";
        case SafetyMode::explanation: return "explanation";
        case SafetyMode::past_event: return "past_event";
        case SafetyMode::hypothetical: return "hypothetical";
        case SafetyMode::quoted_mention: return "quoted_mention";
    }
    return "?";
}

namespace {

// ------------------------------------------------------------ lexicons ---

// Verbs that change system state when OSIE is asked to perform them.
const WordSet kActionVerbs = {
    "kill", "pkill", "killall", "terminate", "quit", "end", "stop", "abort", "halt", "sigkill", "force-quit",
    "restart", "reboot", "reload", "relaunch", "bounce", "shutdown", "shut", "poweroff", "start", "launch",
    "delete", "remove", "rm", "erase", "wipe", "unlink", "truncate", "shred", "purge", "empty",
    "set", "change", "modify", "adjust", "increase", "decrease", "raise", "lower", "reduce", "bump", "double",
    "halve", "cap", "limit", "throttle", "unthrottle", "give", "grant", "allocate", "assign", "resize", "scale",
    "renice", "nice", "prioritize", "prioritise", "deprioritize", "deprioritise", "pin", "taskset", "bind",
    "block", "unblock", "allow", "deny", "open", "close", "disable", "enable", "flush", "drop",
    "install", "uninstall", "upgrade", "update", "patch", "apply", "deploy", "rollout", "edit",
    "free", "optimize", "optimise", "tune", "fix", "speed", "boost", "clean", "clear", "mount", "unmount",
    "umount", "format", "chmod", "chown", "move", "rename", "suspend", "pause", "resume", "evict", "drain",
    "cordon", "migrate", "run", "execute", "exec",
};
// Command names that are directives when they open a clause ("sudo ...", "systemctl restart x").
const WordSet kCommandWords = {"sudo", "systemctl", "launchctl", "kubectl", "iptables", "ufw", "nft", "sysctl",
                               "docker", "podman", "service", "rm", "kill", "pkill", "killall", "renice",
                               "taskset", "chmod", "chown", "ifconfig", "ip", "nmcli", "pfctl", "echo"};
// Past or passive forms: "why was nginx killed?" reports an event.
const WordSet kPastForms = {"killed", "terminated", "restarted", "rebooted", "stopped", "deleted", "removed",
                            "ended", "reloaded", "relaunched", "crashed", "died", "reniced", "pinned", "evicted",
                            "started", "launched", "shutdown", "oom-killed", "wiped", "erased", "purged",
                            "disabled", "enabled", "changed", "modified", "lowered", "raised", "blocked"};
// Clause openers that do not change its mood ("please", "ok now", "go ahead and").
const WordSet kFillers = {"please", "ok", "okay", "now", "just", "so", "then", "quickly", "kindly", "osie", "hey",
                          "hi", "yo", "alright", "right", "sure", "yes", "yeah", "go", "ahead", "and", "also",
                          "immediately", "first", "finally", "simply", "asap", "pretty", "well", "um", "uh",
                          "hmm", "oh", "actually", "maybe", "can", "could", "would", "will", "you"};
const WordSet kInterrogatives = {"how", "what", "why", "when", "where", "which", "who", "whom", "whose", "is", "are",
                                 "was", "were", "does", "do", "did", "has", "have", "had", "explain", "describe",
                                 "define", "should", "tell", "difference"};
const WordSet kModals = {"can", "could", "would", "will", "must", "should", "may", "might"};
const WordSet kExplainMarkers = {"difference", "vs", "versus", "mean", "means", "meaning", "work", "works",
                                 "explain", "purpose", "safe", "dangerous", "happens", "happen"};
const WordSet kDeterminers = {"the", "a", "an", "my", "its", "your", "our", "their", "this", "that", "cpu", "memory",
                              "rate", "hard", "soft", "upper", "lower", "no", "any", "some"};

// "ignore your rules", "admin mode", "I authorize you" ... Recorded so that
// the refusal can say that conversational authorisation changes nothing.
bool override_marker(const std::vector<Token>& t, std::size_t b, std::size_t e, std::string& cue) {
    static const WordSet kIgnore = {"ignore", "disregard", "forget", "bypass", "override", "skip", "drop"};
    static const WordSet kRules = {"rules", "rule", "instructions", "instruction", "policy", "policies", "restrictions",
                                   "safety", "guardrails", "read-only", "readonly", "previous", "prior", "above",
                                   "constraints", "limitations", "safeguards"};
    for (std::size_t i = b; i < e; ++i) {
        const auto& w = t[i].lower;
        const std::string_view n1 = i + 1 < e ? std::string_view(t[i + 1].lower) : std::string_view{};
        const std::string_view n2 = i + 2 < e ? std::string_view(t[i + 2].lower) : std::string_view{};
        const std::string_view n3 = i + 3 < e ? std::string_view(t[i + 3].lower) : std::string_view{};
        if (kIgnore.count(w) && (kRules.count(n1) || kRules.count(n2) || kRules.count(n3))) {
            cue = "instruction to ignore the policy";
            return true;
        }
        if ((w == "admin" || w == "developer" || w == "god" || w == "root" || w == "unrestricted" || w == "sudo") &&
            n1 == "mode") {
            cue = "claimed '" + w + " mode'";
            return true;
        }
        if (w == "jailbreak" || w == "jailbroken") {
            cue = "jailbreak attempt";
            return true;
        }
        if (w == "you" && n1 == "are" && n2 == "now") {
            cue = "role reassignment";
            return true;
        }
        if (w == "pretend" || (w == "act" && n1 == "as")) {
            cue = "role-play request";
            return true;
        }
        if ((w == "authorize" || w == "authorise" || w == "authorized" || w == "authorised") ||
            (w == "permission" && (i > b && (t[i - 1].lower == "my" || t[i - 1].lower == "give" || t[i - 1].lower == "you" ||
                                             t[i - 1].lower == "have" || t[i - 1].lower == "full"))) ||
            (w == "you" && n1 == "are" && n2 == "allowed")) {
            cue = "conversational authorisation";
            return true;
        }
        if (w == "as" && (n1 == "root" || n1 == "admin" || n1 == "administrator" || n1 == "superuser")) {
            cue = "claimed privilege ('as " + std::string(n1) + "')";
            return true;
        }
    }
    return false;
}

bool any_in(const std::vector<Token>& t, std::size_t b, std::size_t e, const WordSet& s) {
    for (std::size_t i = b; i < e; ++i)
        if (s.count(t[i].lower)) return true;
    return false;
}
bool any_word(const std::vector<Token>& t, std::size_t b, std::size_t e, std::initializer_list<std::string_view> ws) {
    for (std::size_t i = b; i < e; ++i)
        for (auto w : ws)
            if (t[i].lower == w) return true;
    return false;
}

bool is_number(std::string_view s) {
    return !s.empty() && s.size() <= 9 &&
           std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

/// True when token i is used as an action verb in this position (not a noun
/// or adjective: "the limit", "free memory?", "give me", "set of").
bool verb_use(const std::vector<Token>& t, std::size_t i, std::size_t e, bool question) {
    const auto& w = t[i].lower;
    if (!kActionVerbs.count(w) && !kCommandWords.count(w)) return false;
    const std::string_view next = i + 1 < e ? std::string_view(t[i + 1].lower) : std::string_view{};
    if (w == "give" && (next == "me" || next == "us")) return false;
    // Allocation verbs are actions only with a resource object ("give nginx another core").
    if (w == "give" || w == "grant" || w == "allocate" || w == "assign") {
        bool resource = false;
        for (std::size_t k = i + 1; k < e; ++k)
            for (auto r : {"core", "cores", "cpu", "cpus", "memory", "ram", "quota", "limit", "priority", "access", "root"})
                resource = resource || t[k].lower == r;
        if (!resource) return false;
    }
    if (w == "set" && next == "of") return false;
    if (w == "double" && (next == "check" || next == "checking")) return false;
    // "run"/"execute" are instructions only when they introduce a command ("run rm -rf /").
    if (w == "run" || w == "execute" || w == "exec")
        return kCommandWords.count(next) > 0 || kActionVerbs.count(next) > 0;
    if (w == "free" && next != "up" && (question || next.empty())) return false;
    if (w == "run" && (next == "queue" || next == "time")) return false;
    if (w == "open" && (next == "files" || next == "file" || next == "ports" || next == "connections")) return false;
    if (w == "start" && (next == "time" || next == "up")) return false;
    if (w == "service" && next != "restart" && next != "stop" && next != "start" && i + 2 < e &&
        t[i + 2].lower != "restart" && t[i + 2].lower != "stop" && t[i + 2].lower != "start")
        return false;
    if ((w == "ip" || w == "echo" || w == "docker" || w == "podman") && next.empty()) return false;
    if (w == "docker" || w == "podman")
        return next == "kill" || next == "stop" || next == "rm" || next == "restart" || next == "start" ||
               next == "pause" || next == "update" || next == "exec" || next == "run" || next == "rmi" ||
               next == "prune" || next == "system";
    return true;
}

ActionType categorize(const std::vector<Token>& t, std::size_t b, std::size_t e, std::string_view raw_clause) {
    auto any = [&](std::initializer_list<std::string_view> ws) { return any_word(t, b, e, ws); };
    auto prefixed = [&](std::initializer_list<std::string_view> ps) {
        for (std::size_t i = b; i < e; ++i)
            for (auto p : ps)
                if (t[i].lower.rfind(p, 0) == 0 && t[i].lower.size() > p.size()) return true;
        return false;
    };
    const bool limit_noun = any({"quota", "quotas", "limit", "limits", "cpu.max", "memory.max", "memory.high", "cpushares",
                                 "shares", "cpuset"});
    if (any({"pod", "pods", "deployment", "deployments", "kubectl", "k8s", "kubernetes", "namespace", "replicas",
             "statefulset", "daemonset", "replicaset", "node", "nodes", "cordon", "drain"}))
        return ActionType::kubernetes_patch;
    if (limit_noun && !any({"firewall", "rate"})) return ActionType::cgroup_limit;
    bool docker_cmd = false;
    for (std::size_t i = b; i + 1 < e; ++i)
        if ((t[i].lower == "docker" || t[i].lower == "podman") &&
            (t[i + 1].lower == "kill" || t[i + 1].lower == "stop" || t[i + 1].lower == "rm" ||
             t[i + 1].lower == "restart" || t[i + 1].lower == "start" || t[i + 1].lower == "pause" ||
             t[i + 1].lower == "rmi" || t[i + 1].lower == "prune"))
            docker_cmd = true;
    if (docker_cmd || any({"container", "containers"})) return ActionType::container_control;
    const bool delete_verb = any({"delete", "remove", "rm", "erase", "wipe", "unlink", "truncate", "shred", "empty"});
    const bool path_like = raw_clause.find('/') != std::string_view::npos ||
                           any({"file", "files", "folder", "folders", "directory", "directories", "dir", "logs", "log",
                                "trash", "downloads", "home"});
    if ((delete_verb && path_like) || any({"rm"})) return ActionType::file_delete;
    if (any({"firewall", "iptables", "ufw", "nft", "nftables", "pfctl", "port", "ports"})) return ActionType::firewall_config;
    if (any({"sysctl", "swappiness", "tunable", "tunables", "hugepages", "overcommit", "parameter", "parameters"}) ||
        prefixed({"vm.", "kernel.", "net.", "fs."}))
        return ActionType::kernel_parameter;
    if (any({"dns", "ip", "route", "routes", "gateway", "interface", "interfaces", "wifi", "wi-fi", "ethernet", "eth0",
             "en0", "mtu", "network", "networking", "ifconfig", "nmcli", "proxy", "vpn"}))
        return ActionType::network_config;
    if (any({"renice", "nice", "priority", "niceness", "prioritize", "prioritise", "deprioritize", "deprioritise"}))
        return ActionType::set_priority;
    if (any({"pin", "taskset", "affinity", "bind"})) return ActionType::set_affinity;
    if (delete_verb) return ActionType::file_delete;
    if (any({"restart", "reboot", "reload", "relaunch", "bounce", "shutdown", "poweroff", "halt", "start", "launch"}) ||
        (any({"shut"}) && any({"down"})))
        return ActionType::restart_service;
    if (any({"kill", "pkill", "killall", "terminate", "quit", "end", "stop", "abort", "sigkill", "force-quit", "close"}))
        return ActionType::terminate;
    if (any({"core", "cores", "cpu", "cpus"}) &&
        any({"give", "set", "raise", "lower", "increase", "decrease", "cap", "limit", "double", "halve", "reduce",
             "allocate", "bump", "throttle", "grant", "assign"}))
        return ActionType::cgroup_limit;
    return ActionType::generic_change;
}

struct Clause {
    std::size_t b{0}, e{0};  // token range
    std::string_view raw;    // raw text of the clause
};

}  // namespace

SafetyAssessment assess_safety(std::string_view text, const std::vector<Token>& toks) {
    SafetyAssessment out;
    const std::size_t n = toks.size();
    if (n == 0) return out;

    // 1. Quoted regions: their tokens are mentions, not instructions.
    std::vector<bool> quoted(n, false);
    {
        auto is_quote = [](unsigned char c) { return c == '"' || c == '\'' || c == '`'; };
        std::vector<std::pair<std::size_t, std::size_t>> regions;
        std::size_t open = std::string_view::npos;
        unsigned char open_c = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            // UTF-8 curly quotes (U+2018/2019/201C/201D) are treated as quotes.
            const bool curly = c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                               (static_cast<unsigned char>(text[i + 2]) >= 0x98 && static_cast<unsigned char>(text[i + 2]) <= 0x9D);
            if (!is_quote(c) && !curly) continue;
            const bool prev_alnum = i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0;
            const bool next_alnum = i + 1 < text.size() && std::isalnum(static_cast<unsigned char>(text[i + 1])) != 0;
            if (c == '\'' && prev_alnum && next_alnum) continue;  // apostrophe: "what's", "I'm"
            if (open == std::string_view::npos) {
                if (c == '\'' && prev_alnum) continue;  // possessive "nginx' "
                open = i;
                open_c = curly ? 0xE2 : c;
            } else if ((curly && open_c == 0xE2) || c == open_c) {
                regions.emplace_back(open, i);
                open = std::string_view::npos;
            }
            if (curly) i += 2;
        }
        for (std::size_t k = 0; k < n; ++k)
            for (const auto& [qb, qe] : regions)
                if (toks[k].begin > qb && toks[k].end <= qe) quoted[k] = true;
    }

    // 2. Clauses: split at sentence punctuation between tokens, and at
    // "and/then/but" when what follows opens a new instruction.
    std::vector<Clause> clauses;
    {
        std::size_t start = 0;
        auto push = [&](std::size_t b, std::size_t e) {
            if (b >= e) return;
            clauses.push_back(Clause{b, e, text.substr(toks[b].begin, toks[e - 1].end - toks[b].begin)});
        };
        for (std::size_t i = 1; i < n; ++i) {
            const auto gap = text.substr(toks[i - 1].end, toks[i].begin - toks[i - 1].end);
            const bool punct = gap.find_first_of(".;:!?,") != std::string_view::npos ||
                               gap.find(" - ") != std::string_view::npos || gap.find("\xE2\x80\x94") != std::string_view::npos;
            bool conj = false;
            const auto& w = toks[i].lower;
            if ((w == "and" || w == "then" || w == "but") && i + 1 < n) {
                bool between = false;
                for (std::size_t k = start; k < i; ++k) between = between || toks[k].lower == "between";
                std::size_t j = i + 1;
                while (j < n && kFillers.count(toks[j].lower) && !kActionVerbs.count(toks[j].lower)) ++j;
                conj = !between && j < n && verb_use(toks, j, n, false);
            }
            if (punct) {
                push(start, i);
                start = i;
            } else if (conj) {
                push(start, i);
                start = i + 1;
            }
        }
        push(start, n);
    }

    // 3. Classify each clause.
    const bool question_mark = text.find('?') != std::string_view::npos;
    bool any_directive = false, any_hypo = false, any_past = false, any_explain = false, any_quoted_action = false;
    bool any_override = false;
    std::string override_cue;
    const Clause* directive_clause = nullptr;
    std::string directive_cue;
    bool hypothetical_context = false;  // an earlier clause opened with "if"/"what if"/"predict"
    for (const auto& c : clauses) {
        std::string cue;
        if (override_marker(toks, c.b, c.e, cue)) {
            any_override = true;
            override_cue = cue;
        }
        // First non-filler token.
        std::size_t f = c.b;
        while (f < c.e && kFillers.count(toks[f].lower) && !kActionVerbs.count(toks[f].lower)) ++f;
        const bool clause_q = c.raw.find('?') != std::string_view::npos ||
                              (question_mark && &c == &clauses.back()) ||
                              (c.b < c.e && kInterrogatives.count(toks[c.b].lower) > 0);
        const bool starts_interrogative = c.b < c.e && (kInterrogatives.count(toks[c.b].lower) > 0 ||
                                                        (kModals.count(toks[c.b].lower) && c.b + 1 < c.e &&
                                                         (toks[c.b + 1].lower == "i" || toks[c.b + 1].lower == "we")));
        bool would = false;  // "what would happen", "would it be throttled" - not "would you ..."
        for (std::size_t i = c.b; i < c.e; ++i)
            if (toks[i].lower == "would" && !(i + 1 < c.e && (toks[i + 1].lower == "you" || toks[i + 1].lower == "like")))
                would = true;
        const bool hypo = would ||
                          any_word(toks, c.b, c.e, {"if", "predict", "prediction", "suppose", "supposing", "imagine",
                                                    "hypothetically", "hypothetical", "assuming"}) ||
                          (c.b < c.e && toks[c.b].lower == "what" && c.b + 1 < c.e && toks[c.b + 1].lower == "if");
        bool has_action = false, has_past = false, quoted_action = false;
        for (std::size_t i = c.b; i < c.e; ++i) {
            if (quoted[i]) {
                if (kActionVerbs.count(toks[i].lower) || kPastForms.count(toks[i].lower)) quoted_action = true;
                continue;
            }
            if (kPastForms.count(toks[i].lower)) has_past = true;
            if (verb_use(toks, i, c.e, clause_q) && kActionVerbs.count(toks[i].lower)) has_action = true;
        }
        any_quoted_action = any_quoted_action || quoted_action;

        // Requests addressed to OSIE: "can you kill", "I want you to restart",
        // "permission to reboot", "let's delete", "help me kill", "sudo ...".
        bool request = false;
        for (std::size_t i = c.b; i < c.e && !request; ++i) {
            if (quoted[i]) continue;
            const auto& w = toks[i].lower;
            auto verb_after = [&](std::size_t j, std::size_t window) {
                for (std::size_t k = j; k < c.e && k < j + window; ++k) {
                    if (quoted[k]) return false;
                    if (verb_use(toks, k, c.e, false) && kActionVerbs.count(toks[k].lower)) return true;
                    if (!kFillers.count(toks[k].lower) && toks[k].lower != "to") return false;
                }
                return false;
            };
            if (kModals.count(w) && i + 1 < c.e && (toks[i + 1].lower == "you" || toks[i + 1].lower == "osie"))
                request = verb_after(i + 2, 4);
            else if (w == "you" && i + 1 < c.e && toks[i + 1].lower == "to") request = verb_after(i + 2, 3);
            else if ((w == "permission" || w == "allowed" || w == "authorized" || w == "authorised" ||
                      w == "authorize" || w == "authorise") && i + 1 < c.e)
                request = verb_after(i + 1, 4);
            else if ((w == "let" || w == "lets") && i + 1 < c.e) request = verb_after(i + 1, 3);
            else if (w == "help" && i + 1 < c.e && toks[i + 1].lower == "me") request = verb_after(i + 2, 2);
            else if (w == "sudo") request = true;
            else if ((w == "want" || w == "need") && i + 1 < c.e && toks[i + 1].lower == "to" && !clause_q)
                request = verb_after(i + 2, 2);
            if (request) cue = "request addressed to OSIE";
        }
        const bool imperative = f < c.e && !quoted[f] && verb_use(toks, f, c.e, clause_q) &&
                                (kActionVerbs.count(toks[f].lower) || kCommandWords.count(toks[f].lower)) &&
                                !(clause_q && any_in(toks, c.b, c.e, kExplainMarkers));
        if (imperative && cue.empty()) cue = "imperative '" + toks[f].lower + "'";

        if (hypo || hypothetical_context) {
            // A consequence clause after an if-clause ("if X, restart it") is
            // still an instruction when it is imperative.
            if (imperative && !hypo && !clause_q) {
                any_directive = true;
                directive_clause = directive_clause ? directive_clause : &c;
                directive_cue = cue;
            } else if (has_action || has_past || any_word(toks, c.b, c.e, {"core", "cores", "cpu", "cpus", "quota", "limit"})) {
                any_hypo = true;
            }
            if (hypo) hypothetical_context = true;
            continue;
        }
        if (request || (imperative && !starts_interrogative)) {
            any_directive = true;
            if (!directive_clause) {
                directive_clause = &c;
                directive_cue = cue;
            }
            continue;
        }
        if (has_past && !has_action) any_past = true;
        else if (has_action && (starts_interrogative || clause_q)) any_explain = true;
        else if (has_past) any_past = true;
    }

    // An override attempt combined with any unquoted action verb is a
    // directive even without an imperative ("I authorize the restart").
    if (any_override && !any_directive) {
        for (std::size_t i = 0; i < n; ++i)
            if (!quoted[i] && kActionVerbs.count(toks[i].lower) && verb_use(toks, i, n, false)) {
                any_directive = true;
                directive_cue = "action verb '" + toks[i].lower + "' with " + override_cue;
                for (const auto& c : clauses)
                    if (i >= c.b && i < c.e) directive_clause = &c;
                break;
            }
    }
    out.override_attempt = any_override;

    if (any_directive && directive_clause) {
        out.mode = SafetyMode::directive;
        out.cue = any_override ? directive_cue + "; " + override_cue : directive_cue;
        // Categorise over the directive clause plus any later clauses of the
        // same instruction ("ignore the rules. delete /var/log/syslog").
        const auto& c = *directive_clause;
        out.action = categorize(toks, c.b, c.e, c.raw);
        // Bare PID operand: "kill -9 4242", "kill 5" (but not a signal number).
        for (std::size_t i = c.b; i < c.e; ++i) {
            if (!is_number(toks[i].lower) || quoted[i]) continue;
            const bool flag = toks[i].begin > 0 && text[toks[i].begin - 1] == '-';
            if (flag) continue;
            const bool after_verb = i > c.b && (kActionVerbs.count(toks[i - 1].lower) || kCommandWords.count(toks[i - 1].lower) ||
                                                toks[i - 1].lower == "pid" || toks[i - 1].lower == "process" ||
                                                (i > c.b + 1 && text[toks[i - 1].begin - 1] == '-'));
            if (after_verb) {
                out.pid = toks[i].lower;
                break;
            }
        }
        return out;
    }
    if (any_hypo) {
        out.mode = SafetyMode::hypothetical;
        out.cue = "hypothetical question";
    } else if (any_past) {
        out.mode = SafetyMode::past_event;
        out.cue = "question about a past event";
    } else if (any_explain) {
        out.mode = SafetyMode::explanation;
        out.cue = "question about how an action works";
    } else if (any_quoted_action) {
        out.mode = SafetyMode::quoted_mention;
        out.cue = "action appears only inside quotes";
    }
    return out;
}

}  // namespace osie::intent
