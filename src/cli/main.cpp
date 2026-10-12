// SPDX-License-Identifier: Apache-2.0
// osie command-line interface.
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "osie/architecture/model.hpp"
#include "osie/engine/engine.hpp"
#include "osie/inference/neural_parser.hpp"
#include "osie/util/format.hpp"
#include "osie/util/json.hpp"

namespace {

using namespace osie;

struct Options {
    std::string command = "repl";
    std::string text;
    // Default is the zero-parameter rule parser: in held-out evaluation no trained
    // model beat it by a margin larger than seed variance (docs/RESEARCH.md §2).
    std::string parser = "rules";  // rules | neural | hybrid
    std::string model_path;
    std::string audit_path;
    long interval_ms = 500;
    bool json = false;
    bool verbose = false;
    bool dot = false;
    bool concepts = false;
};

void usage() {
    std::puts(
        "usage: osie [command] [options]\n"
        "commands:\n"
        "  (none)              interactive session\n"
        "  ask \"<question>\"    answer one question\n"
        "  parse \"<question>\"  show the validated intent frame only (no observation)\n"
        "  snapshot            observe everything supported and print a summary\n"
        "  caps                list capabilities and policy decisions\n"
        "  graph               knowledge-graph statistics (--dot for Graphviz, --concepts for ontology only)\n"
        "options:\n"
        "  --json              machine-readable output\n"
        "  --verbose           include all evidence and documentation references\n"
        "  --parser MODE       rules (default) | hybrid (tiny model + rule fallback) | neural\n"
        "  --model PATH        intent model file (default: $OSIE_MODEL or ./models/intent.osiem)\n"
        "  --interval MS       sampling window for rates (default 500)\n"
        "  --audit FILE        append policy decisions as JSON lines");
}

bool parse_args(int argc, char** argv, Options& o) {
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (a == "--json") o.json = true;
        else if (a == "--verbose" || a == "-v") o.verbose = true;
        else if (a == "--dot") o.dot = true;
        else if (a == "--concepts") o.concepts = true;
        else if (a == "--parser") { if (!next(o.parser)) return false; }
        else if (a == "--model") { if (!next(o.model_path)) return false; }
        else if (a == "--audit") { if (!next(o.audit_path)) return false; }
        else if (a == "--interval") {
            std::string v;
            if (!next(v)) return false;
            o.interval_ms = std::strtol(v.c_str(), nullptr, 10);
            if (o.interval_ms < 50 || o.interval_ms > 10000) return false;
        } else if (a == "-h" || a == "--help") return false;
        else pos.push_back(a);
    }
    if (!pos.empty()) {
        o.command = pos[0];
        for (std::size_t i = 1; i < pos.size(); ++i) o.text += (i > 1 ? " " : "") + pos[i];
    }
    return true;
}

std::string answer_json(const Answer& a) {
    json::Writer w;
    w.begin_object();
    w.key("frame");
    std::string frame = intent::to_json(a.frame);
    std::string diag = a.diagnosis ? reason::to_json(*a.diagnosis) : "null";
    w.null();  // placeholder replaced below (Writer has no raw-insert by design)
    w.kv("text", a.text).kv("latency_ms", a.latency_ms).kv("parse_ms", a.parse_ms)
        .kv("observation_age_ms", static_cast<std::int64_t>(a.observation_age.count()))
        .kv("reused_observations", a.reused_observations);
    w.end_object();
    std::string s = w.take();
    const std::string ph = "\"frame\":null";
    s.replace(s.find(ph), ph.size(), "\"frame\":" + frame + ",\"diagnosis\":" + diag);
    return s;
}

std::unique_ptr<intent::IIntentParser> make_parser(const Options& o, std::string& note) {
    if (o.parser == "rules") return std::make_unique<intent::RuleParser>();
    std::string path = o.model_path;
    if (path.empty()) {
        if (const char* env = std::getenv("OSIE_MODEL")) path = env;
    }
    if (path.empty()) {
        for (const char* cand : {"models/intent.osiem", "../models/intent.osiem", OSIE_DEFAULT_MODEL_PATH}) {
            if (::access(cand, R_OK) == 0) {
                path = cand;
                break;
            }
        }
    }
    if (path.empty()) {
        if (o.parser == "neural" || o.parser == "hybrid") note = "no model file found; using rules";
        return std::make_unique<intent::RuleParser>();
    }
    auto model = inference::IntentModel::load(path);
    if (!model) {
        note = "model load failed (" + model.error().describe() + "); using rules";
        return std::make_unique<intent::RuleParser>();
    }
    auto shared = std::make_shared<inference::IntentModel>(std::move(model).value());
    if (o.parser == "neural") return std::make_unique<inference::NeuralParser>(shared);
    return std::make_unique<inference::HybridParser>(shared);
}

void print_caps(Engine& e, bool as_json) {
    SafetyPolicy pol;
    json::Writer w;
    w.begin_array();
    for (const auto& c : e.registry().all()) {
        auto d = pol.authorize(c, e.os().platform(), e.os().supported());
        if (as_json) {
            w.begin_object().kv("id", c.id).kv("kind", c.kind == CapabilityKind::observe ? "observe" : "act")
                .kv("risk", to_string(c.risk)).kv("platforms", c.platforms.to_string())
                .kv("permissions", c.required_permissions.to_string()).kv("requires_confirmation", c.requires_confirmation)
                .kv("reversible", c.reversible).kv("implemented", c.implemented).kv("allowed", d.allowed)
                .kv("reason", d.reason).end_object();
        } else {
            std::printf("%-34s %-7s risk=%-8s %-12s %s\n", c.id.c_str(), d.allowed ? "ALLOW" : "DENY",
                        std::string(to_string(c.risk)).c_str(), c.platforms.to_string().c_str(), d.reason.c_str());
        }
    }
    w.end_array();
    if (as_json) std::puts(w.str().c_str());
}

void print_graph(Engine& e, const Options& o) {
    if (!o.concepts) e.observe_all();
    const auto& g = e.graph();
    if (o.dot) {
        std::fputs(g.to_dot(o.concepts).c_str(), stdout);
        return;
    }
    auto st = g.stats();
    std::printf("knowledge graph revision %llu: %zu nodes, %zu edges alive\n",
                static_cast<unsigned long long>(g.revision()), st.nodes_alive, st.edges_alive);
    for (std::size_t k = 0; k < static_cast<std::size_t>(arch::NodeKind::count_); ++k) {
        if (st.by_kind[k] == 0) continue;
        std::printf("  %-14s %zu\n", std::string(arch::to_string(static_cast<arch::NodeKind>(k))).c_str(), st.by_kind[k]);
    }
    auto v = g.check_consistency();
    std::printf("consistency: %s\n", v.empty() ? "ok" : (std::to_string(v.size()) + " violations").c_str());
    for (std::size_t i = 0; i < v.size() && i < 10; ++i) std::printf("  ! %s\n", v[i].c_str());
}

void print_snapshot(Engine& e, bool as_json) {
    auto s = e.observe_all();
    if (as_json) {
        json::Writer w;
        w.begin_object();
        if (s->system) w.kv("os", s->system->value.os_name + " " + s->system->value.os_version);
        if (s->cpu) w.kv("cpu_busy_pct", s->cpu->value.busy_pct());
        if (s->load) w.kv("load_1m", s->load->value.one);
        if (s->memory) {
            w.kv("mem_total", s->memory->value.total_bytes).kv("mem_available", s->memory->value.available_bytes);
            if (s->memory->value.pressure_level) w.kv("pressure_level", to_string(*s->memory->value.pressure_level));
        }
        if (s->processes) w.kv("processes", static_cast<std::uint64_t>(s->processes->value.size()))
                              .kv("processes_partial", static_cast<std::uint64_t>(s->processes_partial));
        w.key("unavailable").begin_array();
        for (const auto& u : s->unavailable)
            w.begin_object().kv("kind", to_string(u.kind)).kv("reason", u.why.describe()).end_object();
        w.end_array();
        w.end_object();
        std::puts(w.str().c_str());
        return;
    }
    if (s->system) {
        const auto& si = s->system->value;
        std::string line = si.os_name + " " + si.os_version;
        for (const auto* extra : {&si.hardware_model, &si.cpu_brand})
            if (!extra->empty()) line += ", " + *extra;
        std::printf("%s (kernel %s, %s)\n", line.c_str(), si.kernel_release.c_str(), si.arch.c_str());
    }
    if (s->cpu) std::printf("cpu: %.1f%% busy (user %.1f, sys %.1f) [%s]\n", s->cpu->value.busy_pct(), s->cpu->value.user_pct,
                            s->cpu->value.system_pct, std::string(s->cpu->prov.source).c_str());
    if (s->load) std::printf("load: %.2f %.2f %.2f\n", s->load->value.one, s->load->value.five, s->load->value.fifteen);
    if (s->memory) {
        const auto& m = s->memory->value;
        std::printf("memory: %s total, %s available (est.)", fmt::bytes(m.total_bytes).c_str(),
                    fmt::bytes(m.available_bytes.value_or(0)).c_str());
        if (m.pressure_level) std::printf(", pressure %s", std::string(to_string(*m.pressure_level)).c_str());
        std::printf(" [%s]\n", std::string(s->memory->prov.source).c_str());
    }
    if (s->processes) std::printf("processes: %zu (%u partially readable, %u exited during sampling)\n",
                                  s->processes->value.size(), s->processes_partial, s->processes_vanished);
    if (s->power) std::printf("thermal: %s\n", std::string(to_string(s->power->value.thermal)).c_str());
    if (s->psi) for (const auto& p : s->psi->value)
        std::printf("psi %s: some avg10=%.2f%s\n", std::string(to_string(p.resource)).c_str(), p.some.avg10,
                    p.full ? (" full avg10=" + fmt::num(p.full->avg10, 2)).c_str() : "");
    for (const auto& u : s->unavailable)
        std::printf("unavailable: %s - %s\n", std::string(to_string(u.kind)).c_str(), u.why.describe().c_str());
}

int repl(Engine& e, Options& o, const std::string& note) {
    const bool tty = ::isatty(STDIN_FILENO) != 0;
    if (tty) {
        std::printf("OSIE - OS-native intelligence engine (read-only). Parser: %s%s\nType 'help' or ':quit'.\n",
                    std::string(e.parser().name()).c_str(), note.empty() ? "" : (" (" + note + ")").c_str());
    }
    std::string line;
    while (true) {
        if (tty) std::fputs("osie> ", stdout);
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == ":quit" || line == ":q" || line == "exit" || line == "quit") break;
        if (line == ":json") { o.json = !o.json; std::printf("json output %s\n", o.json ? "on" : "off"); continue; }
        if (line == ":verbose") { o.verbose = !o.verbose; std::printf("verbose %s\n", o.verbose ? "on" : "off"); continue; }
        if (line == ":caps") { print_caps(e, false); continue; }
        if (line == ":graph") { auto oo = o; oo.dot = false; oo.concepts = true; print_graph(e, oo); continue; }
        if (line == ":audit") {
            for (const auto& d : e.audit().recent(15))
                std::printf("%s %-32s %s\n", d.allowed ? "ALLOW" : "DENY ", d.capability.c_str(), d.reason.c_str());
            continue;
        }
        if (line == ":history") {
            for (const auto& [u, i] : e.session().history)
                std::printf("  [%s] %s\n", std::string(intent::to_string(i)).c_str(), u.c_str());
            continue;
        }
        auto a = e.ask(line);
        if (o.json) {
            std::puts(answer_json(a).c_str());
        } else {
            if (!tty) std::printf("osie> %s\n", line.c_str());
            std::fputs("\n", stdout);
            std::fputs((o.verbose && a.diagnosis ? render_diagnosis(*a.diagnosis, true) : a.text).c_str(), stdout);
            std::printf("\n  (%s, intent=%s%s, %.0f ms%s)\n\n", a.frame.parser.c_str(),
                        std::string(intent::to_string(a.frame.intent)).c_str(),
                        a.frame.target.kind != intent::TargetKind::host && a.frame.target.kind != intent::TargetKind::none
                            ? (", target=" + a.frame.target.value).c_str() : "",
                        a.latency_ms, a.reused_observations ? ", reused observations" : "");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        usage();
        return 2;
    }
    std::string note;
    EngineConfig cfg;
    cfg.sample_interval = Millis{o.interval_ms};
    Engine e(make_native_os(), make_parser(o, note), cfg);
    if (!o.audit_path.empty()) {
        if (auto r = e.audit().open_file(o.audit_path); !r) std::fprintf(stderr, "warning: %s\n", r.error().describe().c_str());
    }
    if (!note.empty() && o.command != "repl") std::fprintf(stderr, "note: %s\n", note.c_str());

    if (o.command == "repl") return repl(e, o, note);
    if (o.command == "ask") {
        if (o.text.empty()) { usage(); return 2; }
        auto a = e.ask(o.text);
        if (o.json) std::puts(answer_json(a).c_str());
        else std::fputs((o.verbose && a.diagnosis ? render_diagnosis(*a.diagnosis, true) : a.text).c_str(), stdout);
        return 0;
    }
    if (o.command == "parse") {
        if (o.text.empty()) { usage(); return 2; }
        std::puts(intent::to_json(e.interpret(o.text)).c_str());
        return 0;
    }
    if (o.command == "snapshot") { print_snapshot(e, o.json); return 0; }
    if (o.command == "caps") { print_caps(e, o.json); return 0; }
    if (o.command == "graph") { print_graph(e, o); return 0; }
    usage();
    return 2;
}
