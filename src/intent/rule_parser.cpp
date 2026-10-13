// SPDX-License-Identifier: Apache-2.0
// Deterministic baseline parser: keyword cues + pattern-based slot extraction.
// It has zero trained parameters and is the fallback when no model is loaded.
#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "osie/intent/intent.hpp"

namespace osie::intent {
namespace {

using WordSet = std::unordered_set<std::string_view>;

const WordSet kCpu = {"cpu", "cpus", "processor", "processors", "core", "cores", "load", "compute", "cycles", "utilization", "utilisation"};
const WordSet kMem = {"memory", "ram", "mem", "swap", "swapping", "paging", "rss", "footprint", "leak", "leaking", "gb", "gigabytes", "megabytes"};
const WordSet kDisk = {"disk", "disks", "storage", "drive", "ssd", "volume", "volumes", "filesystem", "filesystems", "partition", "mount", "mounts", "space"};
const WordSet kNet = {"network", "networking", "wifi", "wi-fi", "ethernet", "interface", "interfaces", "bandwidth", "internet", "packets", "traffic", "en0", "eth0"};
const WordSet kThermal = {"thermal", "temperature", "overheating", "overheat", "overheated", "fan", "fans", "heat", "warm", "toasty"};
const WordSet kWhy = {"why", "explain", "cause", "causing", "caused", "reason", "diagnose", "diagnosis", "troubleshoot", "investigate", "wrong", "problem", "issue"};
const WordSet kSlow = {"slow", "slowly", "slowing", "slower", "slows", "bottleneck", "bottlenecks", "bottlenecked", "sluggish", "laggy", "lag", "lagging", "unresponsive", "freezing", "freezes", "frozen", "beachball", "beachballing", "hanging", "hangs", "stutter", "stuttering", "crawling", "performance", "slowdown", "choppy"};
const WordSet kUsage = {"using", "uses", "use", "consuming", "consumes", "consume", "hogging", "hogs", "hog", "eating", "eats", "taking", "takes", "burning", "draining", "chewing", "occupying"};
const WordSet kTop = {"top", "most", "biggest", "highest", "heaviest", "largest", "hungriest", "greediest", "worst"};
const WordSet kK8s = {"kubernetes", "k8s", "pod", "pods", "kubectl", "deployment", "deployments", "replicas", "replicaset", "daemonset", "statefulset", "kubelet", "helm", "hpa"};
const WordSet kContainer = {"container", "containers", "docker", "cgroup", "cgroups", "podman", "containerd"};
const WordSet kThrottle = {"throttled", "throttling", "throttle", "throttles", "quota", "limited", "capped"};
const WordSet kTree = {"parent", "parents", "child", "children", "spawned", "spawn", "spawns", "owns", "own", "owner", "owned", "launched", "started", "tree", "subprocess", "subprocesses", "belongs", "belong", "manages", "managed", "manage"};
const WordSet kSysInfo = {"os", "macos", "linux", "version", "kernel", "uptime", "hostname", "model", "hardware", "build", "darwin", "distro", "distribution", "booted", "boot"};
const WordSet kTopology = {"cores", "chip", "processor", "cpus", "threads", "cache", "architecture", "arm64", "x86_64", "efficiency", "performance"};
const WordSet kAction = {"kill", "terminate", "quit", "pkill", "killall", "renice", "restart", "reboot", "relaunch", "reload",
                         "set", "change", "increase", "decrease", "raise", "lower", "pin", "disable", "enable", "purge",
                         "flush", "clear", "optimize", "optimise", "tune", "fix", "prioritize", "deprioritize", "stop",
                         "close", "free", "limit", "boost", "speed", "apply", "modify", "adjust", "shut", "end", "force-quit"};
const WordSet kProcNouns = {"processes", "programs", "apps", "applications", "tasks", "procs"};
const WordSet kListCue = {"running", "list", "show", "display", "all", "open", "alive", "count", "number", "every",
                          "enumerate"};
const WordSet kCapacityCue = {"allowed", "available", "usable", "entitled", "effective", "actually", "capacity",
                              "can", "able"};
const WordSet kCgroupCue = {"which", "show", "list", "any", "limits", "constrained", "hitting", "what", "apply",
                            "applies", "all"};
const WordSet kHealth = {"healthy", "health", "unhealthy", "bottleneck", "bottlenecks", "bottlenecked"};
const WordSet kHostWords = {"mac", "macbook", "imac", "computer", "machine", "system", "laptop", "server", "box", "host", "pc", "device", "os", "workstation", "desktop"};

// Words that are never part of a process name in slot extraction.
const WordSet kStop = {
    "the", "a", "an", "my", "your", "our", "this", "that", "these", "those", "it", "its", "so", "much", "too", "very",
    "all", "any", "some", "of", "on", "in", "for", "to", "and", "or", "is", "are", "was", "were", "be", "been", "being",
    "do", "does", "did", "why", "what", "which", "who", "how", "when", "where", "using", "use", "uses", "used",
    "consuming", "consume", "consumes", "taking", "take", "takes", "eating", "eats", "hogging", "hogs", "running",
    "run", "runs", "process", "processes", "app", "apps", "application", "applications", "program", "programs",
    "cpu", "cpus", "memory", "ram", "mem", "disk", "network", "swap", "me", "i", "about", "tell", "show", "explain",
    "info", "information", "details", "detail", "pid", "now", "right", "currently", "up", "lot", "many", "more",
    "most", "there", "here", "still", "always", "again", "today", "suddenly", "slow", "hot", "high", "usage",
    "utilization", "time", "load", "much", "heavy", "busy", "crazy", "insane", "huge", "massive", "constantly",
    "with", "by", "from", "at", "as", "s", "t", "just", "really", "even", "only", "should", "could", "would", "can",
    "might", "may", "will", "please", "you", "we", "they", "them", "he", "she", "has", "have", "had", "get", "got",
    "service", "services", "daemon", "thread", "threads", "child", "children", "parent", "doing", "going",
    "one", "first", "second", "third", "last", "same", "other", "else", "not", "no", "yes", "if", "then", "than",
    "like", "into", "out", "over", "under", "behind", "responsible", "culprit", "keep", "keeps", "need", "needs",
    "eat", "hog", "consumption", "resources", "resource", "percent", "%", "level", "levels", "space", "storage",
    "kill", "terminate", "quit", "restart", "renice", "inspect", "check", "investigate", "examine", "analyze",
    "analyse", "owns", "own", "owner", "spawned", "started", "launched", "fan", "fans", "hard", "lots", "mac", "macbook",
    "computer", "machine", "system", "laptop", "server", "host", "pc", "mine", "whole", "entire", "total",
    "every", "each", "anything", "something", "everything", "thing", "things", "stuff", "background",
    "leak", "leaking", "footprint", "rss", "big", "large", "small", "growing", "grow", "grows", "increasing",
    "number", "count", "list", "top", "biggest", "highest", "heaviest", "largest", "set", "change", "make",
    "priority", "affinity", "limit", "quota", "stop", "close", "free", "end", "shut", "down", "via", "per",
    "container", "containers", "cgroup", "cgroups", "vm", "box", "everything", "anything", "nothing", "being"};

const WordSet kPrepBefore = {"about", "of", "for", "on", "by", "kill", "terminate", "quit", "restart", "renice",
                              "end", "pkill", "killall", "reload", "if", "limit", "killed", "terminated",
                              "inspect", "check", "investigate", "examine", "analyze", "analyse", "pid", "is", "does",
                              "did", "are", "was", "named", "called", "relaunch", "close", "stop", "pin", "prioritize",
                              "deprioritize", "spawned", "started", "launched", "owns"};
const WordSet kNameTerminators = {"using", "use", "uses", "consuming", "consume", "consumes", "taking", "take",
                                  "eating", "eat", "hogging", "hog", "running", "so", "still", "doing", "have", "has",
                                  "at", "need", "needs", "keep", "keeps", "constantly", "always", "process", "app",
                                  "application", "daemon", "service", "s", "in", "on", "right", "now", "currently",
                                  "taking", "spawning", "burning", "draining", "chewing", "with", "from", "and", "or",
                                  "to", "is", "are", "was", "for", "slow", "stuck", "hung", "frozen", "suddenly",
                                  "such", "this", "that", "up", "consumption", "usage", "memory", "cpu", "ram",
                                  "gets", "get", "got", "another", "would", "will", "being", "throttled", "limited",
                                  "be", "more", "than", "running", "memory", "processes", "killed", "terminated",
                                  "restarted", "stopped", "crashed", "died", "started", "spawned", "get", "got"};

bool any_of(const std::vector<Token>& t, const WordSet& s) {
    return std::any_of(t.begin(), t.end(), [&](const Token& x) { return s.count(x.lower) > 0; });
}
bool has(const std::vector<Token>& t, std::string_view w) {
    return std::any_of(t.begin(), t.end(), [&](const Token& x) { return x.lower == w; });
}
bool has_seq(const std::vector<Token>& t, std::initializer_list<std::string_view> seq) {
    const std::size_t n = seq.size();
    for (std::size_t i = 0; i + n <= t.size(); ++i) {
        std::size_t j = 0;
        for (auto w : seq) {
            if (t[i + j].lower != w) break;
            ++j;
        }
        if (j == n) return true;
    }
    return false;
}
bool is_number(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}
bool is_name_token(const Token& t) {
    return kStop.count(t.lower) == 0 && !is_number(t.lower) && kHostWords.count(t.lower) == 0;
}

}  // namespace

std::vector<Span> RuleParser::extract_spans(const std::vector<Token>& toks) {
    std::vector<Span> spans;
    const std::size_t n = toks.size();
    std::vector<bool> used(n, false);
    auto mark = [&](SpanType ty, std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            if (used[i]) return;
        for (std::size_t i = b; i < e; ++i) used[i] = true;
        spans.push_back(Span{ty, b, e});
    };

    // PID: "pid 123", "process 123", "process id 123"
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const auto& w = toks[i].lower;
        if ((w == "pid" || w == "process" || w == "id" || w == "pids") && is_number(toks[i + 1].lower)) {
            mark(SpanType::pid, i + 1, i + 2);
        }
    }
    // NUM: "top 5", "5 processes", "three apps"
    static const WordSet kCountNouns = {"processes", "apps", "applications", "programs", "tasks", "consumers", "items", "entries"};
    static const WordSet kNumberWords = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "twenty"};
    for (std::size_t i = 0; i < n; ++i) {
        const bool numeric = (is_number(toks[i].lower) && toks[i].lower.size() <= 2) || kNumberWords.count(toks[i].lower);
        if (!numeric || used[i]) continue;
        const bool after_top = i > 0 && toks[i - 1].lower == "top";
        const bool before_noun = i + 1 < n && kCountNouns.count(toks[i + 1].lower);
        if (after_top || before_noun) mark(SpanType::num, i, i + 1);
    }
    // REF: anaphora and ordinal references to earlier results.
    for (std::size_t i = 0; i < n; ++i) {
        if (used[i]) continue;
        const auto& w = toks[i].lower;
        const std::string_view next = i + 1 < n ? std::string_view(toks[i + 1].lower) : std::string_view{};
        const std::string_view next2 = i + 2 < n ? std::string_view(toks[i + 2].lower) : std::string_view{};
        if ((w == "that" || w == "this" || w == "the") &&
            (next == "one" || next == "process" || next == "app" || next == "application" || next == "program") &&
            next2 != "list" && next2 != "tree" && next2 != "table" && next2 != "count") {
            mark(SpanType::ref, i, i + 2);
        } else if (w == "the" && parse_ordinal(next) && (next2 == "one" || next2 == "process" || next2 == "app")) {
            mark(SpanType::ref, i, i + 3);
        } else if (w == "the" && parse_ordinal(next)) {
            mark(SpanType::ref, i, i + 2);
        } else if ((w == "these" || w == "those") && (next == "processes" || next == "apps" || next == "ones")) {
            mark(SpanType::ref, i, i + 2);
        } else if (w == "it" || w == "them" || w == "its") {
            mark(SpanType::ref, i, i + 1);
        } else if (w == "same" && (next == "process" || next == "one" || next == "app")) {
            mark(SpanType::ref, i, i + 2);
        }
    }
    // PROC: name after a cue word, stopping at a terminator.
    auto take_name = [&](std::size_t start) -> bool {
        std::size_t i = start;
        while (i < n && (toks[i].lower == "the" || toks[i].lower == "my" || toks[i].lower == "this" ||
                         toks[i].lower == "that" || toks[i].lower == "process" || toks[i].lower == "app" ||
                         toks[i].lower == "named" || toks[i].lower == "called")) {
            ++i;
        }
        std::size_t e = i;
        while (e < n && e - i < 4 && kNameTerminators.count(toks[e].lower) == 0 && !used[e] && !is_number(toks[e].lower) &&
               (is_name_token(toks[e]) || (e > i && kStop.count(toks[e].lower) == 0))) {
            ++e;
        }
        if (e > i && is_name_token(toks[i])) {
            mark(SpanType::proc, i, e);
            return true;
        }
        return false;
    };
    static const WordSet kCopula = {"is", "are", "does", "did", "was"};
    static const WordSet kVerbAfterName = {"using", "use", "uses", "consuming", "consume", "taking", "take", "eating",
                                           "eat", "hogging", "hog", "running", "so", "still", "doing", "spawning",
                                           "burning", "draining", "chewing", "slow", "stuck", "hung", "frozen", "always",
                                           "constantly", "keep", "keeps", "need", "needs", "have", "has", "at", "process",
                                           "app", "s", "such", "suddenly", "spiking", "maxing", "pegged", "hitting",
                                           "being", "throttled", "limited", "running", "restarted", "killed",
                                           "stopped", "growing", "get", "got", "spawn", "spawned", "start",
                                           "started"};
    for (std::size_t i = 0; i < n; ++i) {
        if (used[i] || !kPrepBefore.count(toks[i].lower) || i + 1 >= n) continue;
        // "X of NAME" names a process only when X is something a process has
        // ("the parent of nginx", "memory of chrome"), not "the capital of peru".
        static const WordSet kOfHeads = {"usage", "memory", "cpu", "parent", "parents", "children", "child", "owner",
                                         "details", "detail", "status", "info", "information", "pid", "threads",
                                         "cgroup", "limits", "limit", "priority", "consumption", "footprint", "rss",
                                         "state", "path", "ram", "load", "quota", "instances", "copies", "all"};
        if (toks[i].lower == "of" && (i == 0 || !kOfHeads.count(toks[i - 1].lower))) continue;
        if (kCopula.count(toks[i].lower)) {
            // After a copula a name must be followed by a verb-like cue ("why is chrome using ...");
            // "when two applications are open" must not yield a process called "open".
            const std::size_t before = spans.size();
            if (!take_name(i + 1)) continue;
            const Span sp = spans.back();
            if (sp.end >= n || !kVerbAfterName.count(toks[sp.end].lower)) {
                for (std::size_t k = sp.begin; k < sp.end; ++k) used[k] = false;
                spans.resize(before);
                continue;
            }
            break;
        }
        if (take_name(i + 1)) break;
    }
    bool have_proc = std::any_of(spans.begin(), spans.end(), [](const Span& s) { return s.type == SpanType::proc; });
    if (!have_proc) {
        // "<name> process/app/daemon", "<name>'s memory", or name followed by a usage verb.
        for (std::size_t i = 0; i + 1 < n && !have_proc; ++i) {
            const auto& nx = toks[i + 1].lower;
            const bool cue = nx == "process" || nx == "app" || nx == "daemon" || nx == "application" || nx == "s" ||
                             nx == "doing" || kUsage.count(nx) > 0;
            if (cue && is_name_token(toks[i]) && !used[i]) {
                std::size_t b = i;
                while (b > 0 && i - b < 3 && is_name_token(toks[b - 1]) && !used[b - 1]) --b;
                mark(SpanType::proc, b, i + 1);
                have_proc = true;
            }
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
    return spans;
}

ParseOutput RuleParser::parse(const std::vector<Token>& t) const {
    ParseOutput o;
    o.parser = "rules";
    o.spans = extract_spans(t);
    if (t.empty()) {
        o.intent = Intent::unsupported;
        return o;
    }
    const bool proc_target = std::any_of(o.spans.begin(), o.spans.end(), [](const Span& s) {
        return s.type == SpanType::proc || s.type == SpanType::pid || s.type == SpanType::ref;
    });
    const bool named_target = std::any_of(o.spans.begin(), o.spans.end(), [](const Span& s) {
        return s.type == SpanType::proc || s.type == SpanType::pid;
    });
    const bool cpu = any_of(t, kCpu) || has(t, "hot");
    const bool mem = any_of(t, kMem);
    const bool why = any_of(t, kWhy) || has_seq(t, {"what", "is", "causing"}) || has_seq(t, {"how", "come"});
    const bool slow = any_of(t, kSlow) && !has_seq(t, {"performance", "cores"}) && !has_seq(t, {"performance", "core"});
    const bool usage = any_of(t, kUsage);
    const bool top = any_of(t, kTop);
    const bool question = has(t, "what") || has(t, "which") || has(t, "who") || has(t, "show") || has(t, "list");
    const auto& first = t.front().lower;
    auto set = [&o](Intent i, float c) {
        o.intent = i;
        o.confidence = c;
        return o;
    };

    static const WordSet kGreeting = {"hi", "hello", "hey", "thanks", "thank"};
    if (t.size() == 1 && (first == "cores" || first == "cpus" || first == "topology")) return set(Intent::cpu_topology, 0.6F);
    if (has(t, "help") || has_seq(t, {"what", "can", "you", "do"}) || has(t, "commands") ||
        (t.size() <= 2 && kGreeting.count(first))) {
        return set(Intent::help, 0.9F);
    }
    if (any_of(t, kK8s)) return set(Intent::kubernetes, 0.9F);
    if (has_seq(t, {"what", "if"}) || has_seq(t, {"what", "happens", "if"}) || has_seq(t, {"what", "would", "happen"}) ||
        has_seq(t, {"what", "will", "happen"}) || ((has(t, "if") && (has(t, "change") || has(t, "increase") || has(t, "decrease") ||
        has(t, "lower") || has(t, "raise") || has(t, "set"))) && (question || has(t, "would") || has(t, "happen")))) {
        return set(Intent::what_if, 0.85F);
    }
    // Imperative or requested action.
    {
        const bool lead = kAction.count(first) > 0 || (t.size() > 1 && (first == "please" || first == "pls") && kAction.count(t[1].lower));
        bool requested = false;
        for (std::size_t i = 0; i + 2 < t.size(); ++i) {
            const bool ask = (t[i].lower == "can" || t[i].lower == "could" || t[i].lower == "should" || t[i].lower == "would" ||
                              t[i].lower == "want" || t[i].lower == "need" || t[i].lower == "help" || t[i].lower == "go") ;
            if (!ask) continue;
            for (std::size_t j = i + 1; j < std::min(t.size(), i + 4); ++j)
                if (kAction.count(t[j].lower) && t[j].lower != "free") requested = true;
        }
        if ((lead || requested) && !(first == "free" && !has(t, "up")) && !why) return set(Intent::action_request, 0.85F);
    }
    // Effective CPU capacity: "how many cpus can this container use?",
    // "what's my effective cpu limit?" (constraints, not hardware).
    if ((cpu || has(t, "cpus")) && !why &&
        ((has_seq(t, {"how", "many"}) || has_seq(t, {"how", "much"}) || has(t, "effective") || has(t, "capacity")) &&
         any_of(t, kCapacityCue)) && !(has(t, "do") && has(t, "have") && !has(t, "effective") && !has(t, "capacity")))
        return set(Intent::cpu_capacity, 0.8F);
    if ((cpu || has(t, "cpus")) && has(t, "available") && (has(t, "me") || has(t, "us")))
        return set(Intent::cpu_capacity, 0.6F);
    // Cgroup overview: "which cgroup is constrained?", "show cgroup limits".
    if (!proc_target && !why && (any_of(t, kContainer) || (has(t, "limits") && !mem)) && any_of(t, kCgroupCue) &&
        !(any_of(t, kThrottle) && has(t, "my")))
        return set(Intent::cgroup_status, 0.8F);
    if (!named_target && any_of(t, kThrottle) && (has(t, "anything") || has(t, "any")) && !why)
        return set(Intent::cgroup_status, 0.6F);
    if (any_of(t, kContainer) && (any_of(t, kThrottle) || slow || cpu || why)) return set(Intent::container_throttle, 0.85F);
    if (proc_target && any_of(t, kThrottle) && !mem) return set(Intent::container_throttle, 0.8F);
    if (any_of(t, kThrottle) && (has(t, "my") || has(t, "being")) && !cpu) return set(Intent::container_throttle, 0.6F);

    // Process enumeration: "what processes are running?", "show all processes",
    // "what's currently running?", "how many processes do I have", "ps".
    {
        const bool noun = any_of(t, kProcNouns) || has(t, "process");
        const bool metric = cpu || mem || top || usage || slow || any_of(t, kDisk) || any_of(t, kNet);
        const bool list_cue = any_of(t, kListCue) || has_seq(t, {"how", "many"});
        const bool bare_running = (has(t, "running") || has(t, "alive")) &&
                                  (has(t, "what") || has(t, "anything") || has(t, "whats")) && !proc_target;
        if ((t.size() == 1 && first == "ps") ||
            (!metric && !why && !any_of(t, kTree) && !proc_target && ((noun && list_cue) || bare_running)))
            return set(Intent::list_processes, 0.8F);
    }
    // Overall health / bottleneck questions are answered by the all-resource diagnosis.
    if (!proc_target && (any_of(t, kHealth) ||
                         ((has(t, "ok") || has(t, "okay") || has(t, "fine")) &&
                          (any_of(t, kHostWords) || has(t, "everything"))) ||
                         (has(t, "pressure") && !mem && !cpu && !any_of(t, kDisk)) ||
                         (has(t, "how") && has(t, "doing") && any_of(t, kHostWords))))
        return set(Intent::diagnose_slow, 0.75F);
    const bool explain_q = has_seq(t, {"how", "does"}) || has_seq(t, {"how", "do"}) || has_seq(t, {"what", "is"}) ||
                           has_seq(t, {"what", "are"}) || has_seq(t, {"what", "does"}) || first == "explain" ||
                           has_seq(t, {"what", "s"}) || has(t, "mean") || has(t, "meaning") || has_seq(t, {"tell", "me", "about"}) ||
                           has_seq(t, {"how", "is"}) || has(t, "difference") || has(t, "affect") || has(t, "affects");
    const auto concept_key = find_concept(t);
    if (concept_key && explain_q && !usage && !(proc_target && !has(t, "workload"))) {
        return set(Intent::explain_concept, 0.8F);
    }
    if (any_of(t, kThermal) && !(why && cpu && !any_of(t, kThermal))) {
        if (why && cpu && !has(t, "fan") && !has(t, "fans")) return set(Intent::diagnose_cpu, 0.7F);
        return set(Intent::thermal_status, 0.8F);
    }
    if (any_of(t, kTree) && (proc_target || has(t, "service") || has(t, "services") || has(t, "processes") || has(t, "process"))) {
        return set(Intent::process_tree, 0.8F);
    }
    if (proc_target) {
        if ((why || usage || has_seq(t, {"how", "much"}) || slow) && mem) return set(Intent::process_memory, 0.85F);
        if ((why || usage || has_seq(t, {"how", "much"}) || slow) && cpu) return set(Intent::process_cpu, 0.85F);
        if (why && slow) return set(Intent::process_cpu, 0.5F);
        if (why || usage) return set(mem ? Intent::process_memory : Intent::process_cpu, 0.55F);
        static const WordSet kInfoCue = {"tell", "show", "info", "information", "details", "describe", "what", "which",
                                         "who", "status", "inspect", "check", "examine", "is", "pid", "about"};
        const bool pid_target = std::any_of(o.spans.begin(), o.spans.end(), [](const Span& s) { return s.type == SpanType::pid; });
        const bool info_cue = pid_target || (any_of(t, kInfoCue) && !has(t, "write") && !has(t, "poem") && !has(t, "joke"));
        if (!(question && top) && info_cue && (pid_target || has(t, "tell") || has(t, "show") || question ||
                                               has(t, "info") || has(t, "details") || has(t, "describe") ||
                                               has(t, "inspect") || has(t, "check") || has(t, "examine") || has(t, "status") ||
                                               has(t, "running") || has(t, "alive") || first == "is" || first == "are"))
            return set(Intent::process_info, 0.6F);
    }
    if (!proc_target && !why && (has(t, "hogs") || has(t, "hog") || has(t, "hoggers")) && t.size() <= 4) {
        if (mem && !cpu) return set(Intent::top_memory, 0.7F);
        if (cpu && !mem) return set(Intent::top_cpu, 0.7F);
    }
    if ((question || top) && (usage || top) && !why) {
        if (mem && !cpu) return set(Intent::top_memory, 0.85F);
        if (cpu || has(t, "resources")) return set(Intent::top_cpu, 0.85F);
        if (any_of(t, kDisk)) return set(Intent::disk_usage, 0.6F);
        return set(Intent::top_cpu, 0.5F);
    }
    if (why || has(t, "high") || has(t, "spiking") || has(t, "pegged") || has(t, "maxed") || has(t, "100")) {
        if (mem && !cpu) return set(Intent::diagnose_memory, 0.85F);
        if (cpu && !mem) return set(Intent::diagnose_cpu, 0.85F);
        if (slow || (!cpu && !mem)) return set(Intent::diagnose_slow, 0.75F);
    }
    if (slow) return set(Intent::diagnose_slow, 0.8F);
    if (any_of(t, kDisk)) return set(Intent::disk_usage, 0.8F);
    if (any_of(t, kNet)) return set(Intent::network_status, 0.8F);
    if (mem && (has_seq(t, {"how", "much"}) || has(t, "free") || has(t, "available") || has(t, "left") || has(t, "installed") ||
                has(t, "total") || has(t, "status") || has(t, "used") || has(t, "pressure") || question || has(t, "is"))) {
        if (has(t, "pressure") && (has(t, "high") || why)) return set(Intent::diagnose_memory, 0.7F);
        return set(Intent::memory_status, 0.8F);
    }
    if (any_of(t, kTopology) && (has_seq(t, {"how", "many"}) || question || has(t, "kind"))) return set(Intent::cpu_topology, 0.8F);
    if (any_of(t, kSysInfo)) return set(Intent::system_info, 0.75F);
    if (cpu && question) return set(Intent::top_cpu, 0.5F);
    if (cpu) return set(Intent::diagnose_cpu, 0.4F);
    if (mem) return set(Intent::memory_status, 0.4F);
    if (concept_key) return set(Intent::explain_concept, 0.5F);
    return set(Intent::unsupported, 0.3F);
}

}  // namespace osie::intent
