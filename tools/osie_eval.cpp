// SPDX-License-Identifier: Apache-2.0
// osie_eval: end-to-end evaluation of intent parsers through the *same*
// deterministic frame builder the engine uses.
//
//   osie_eval --data FILE.tsv --parser rules|neural|hybrid|override [--model M]
//             [--intents PRED.txt] [--errors OUT.tsv] [--json]
//   osie_eval --dump-features "text" --buckets N --ngram 2 4
//   osie_eval --dump-probs --model M --data FILE.tsv     (golden parity with Python)
//
// TSV columns: id, group, text, intent, target_kind, target_value, concept, action
// A frame is end-to-end correct when intent, target kind, target value (names
// and PIDs, case/space-insensitive), reference ordinal, concept and action all match.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "osie/inference/neural_parser.hpp"
#include "osie/intent/intent.hpp"
#include "osie/util/json.hpp"

using namespace osie;

namespace {

struct Row {
    std::string id, group, text, intent, target_kind, target_value, concept_key, action;
};

std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> v;
    std::size_t s = 0;
    while (true) {
        auto t = line.find('\t', s);
        v.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
        if (t == std::string::npos) break;
        s = t + 1;
    }
    return v;
}

std::string norm(std::string s) {
    std::string o;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!o.empty() && o.back() != ' ') o += ' ';
            continue;
        }
        o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::vector<Row> load(const std::string& path) {
    std::ifstream in(path);
    std::vector<Row> rows;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto f = split_tab(line);
        if (header && !f.empty() && f[0] == "id") {
            header = false;
            continue;
        }
        header = false;
        f.resize(8);
        rows.push_back(Row{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]});
    }
    return rows;
}

std::optional<int> ordinal_of(const std::string& v) {
    auto toks = intent::tokenize(v);
    for (const auto& t : toks)
        if (auto o = intent::parse_ordinal(t.lower)) return *o;
    return std::nullopt;
}

double pct(std::size_t a, std::size_t b) { return b == 0 ? 0.0 : 100.0 * static_cast<double>(a) / static_cast<double>(b); }

}  // namespace

int main(int argc, char** argv) {
    std::string data, parser = "rules", model_path, intents_path, errors_path;
    bool as_json = false, dump_probs = false;
    std::string dump_text;
    std::uint32_t buckets = 16384;
    int nmin = 2, nmax = 4;
    bool knowledge = false;
    std::optional<float> threshold;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") data = next();
        else if (a == "--parser") parser = next();
        else if (a == "--model") model_path = next();
        else if (a == "--intents") intents_path = next();
        else if (a == "--errors") errors_path = next();
        else if (a == "--json") as_json = true;
        else if (a == "--dump-probs") dump_probs = true;
        else if (a == "--dump-features") dump_text = next();
        else if (a == "--buckets") buckets = static_cast<std::uint32_t>(std::stoul(next()));
        else if (a == "--ngram") { nmin = std::stoi(next()); nmax = std::stoi(next()); }
        else if (a == "--knowledge") knowledge = true;
        else if (a == "--threshold") threshold = std::stof(next());
    }

    if (!dump_text.empty()) {
        for (const auto& t : intent::tokenize(dump_text)) {
            std::printf("%s", t.text.c_str());
            for (auto f : inference::token_features(t.text, buckets, nmin, nmax, knowledge)) std::printf(" %u", f);
            std::printf("\n");
        }
        return 0;
    }

    std::shared_ptr<inference::IntentModel> model;
    if (!model_path.empty()) {
        auto m = inference::IntentModel::load(model_path);
        if (!m) {
            std::fprintf(stderr, "model: %s\n", m.error().describe().c_str());
            return 1;
        }
        model = std::make_shared<inference::IntentModel>(std::move(m).value());
    }
    auto rows = load(data);
    if (rows.empty()) {
        std::fprintf(stderr, "no rows in %s\n", data.c_str());
        return 1;
    }

    if (dump_probs) {
        for (const auto& r : rows) {
            auto out = model->forward(intent::tokenize(r.text));
            std::printf("%s", r.id.c_str());
            for (float p : out.intent_probs) std::printf(" %.6f", static_cast<double>(p));
            std::printf("\n");
        }
        return 0;
    }

    std::vector<std::string> overrides;
    if (parser == "override") {
        std::ifstream in(intents_path);
        for (std::string l; std::getline(in, l);) overrides.push_back(l);
        if (overrides.size() != rows.size()) {
            std::fprintf(stderr, "override count %zu != rows %zu\n", overrides.size(), rows.size());
            return 1;
        }
    }

    std::unique_ptr<intent::IIntentParser> p;
    intent::RuleParser rules;
    if (parser == "neural" && model) p = std::make_unique<inference::NeuralParser>(model);
    else if (parser == "hybrid" && model) p = std::make_unique<inference::HybridParser>(model, threshold);
    else if (parser == "rules" || parser == "override") p = std::make_unique<intent::RuleParser>();
    else {
        std::fprintf(stderr, "parser %s needs --model\n", parser.c_str());
        return 1;
    }

    // A fixed "full" observation set so frame building is platform-neutral.
    ObservationSet sup;
    for (unsigned k = 0; k < static_cast<unsigned>(ObservationKind::count_); ++k) sup.add(static_cast<ObservationKind>(k));
    const auto reg = CapabilityRegistry::builtin();

    std::size_t n = 0, ok_intent = 0, ok_kind = 0, ok_target = 0, ok_e2e = 0, valid = 0;
    std::size_t intent_ok_but_wrong_target = 0;
    std::map<std::string, std::pair<std::size_t, std::size_t>> per_intent;  // gold -> (correct e2e, total)
    std::map<std::string, std::size_t> confusions;
    std::vector<double> lat_us;
    std::ofstream err;
    if (!errors_path.empty()) {
        err.open(errors_path);
        err << "id\ttext\tgold_intent\tpred_intent\tgold_target\tpred_target\n";
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const auto t0 = std::chrono::steady_clock::now();
        auto toks = intent::tokenize(r.text);
        auto out = p->parse(toks);
        if (parser == "override") {
            if (auto in = intent::intent_from_string(overrides[i])) out.intent = *in;
            out.parser = "override+rule_slots";
        }
        auto f = intent::build_frame(out, toks, Platform::linux_os, sup, reg);
        f.validation_errors = intent::validate(f);
        lat_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());

        ++n;
        if (f.validation_errors.empty()) ++valid;
        const std::string pi(intent::to_string(f.intent));
        const std::string pk(intent::to_string(f.target.kind));
        const bool i_ok = pi == r.intent;
        const bool k_ok = pk == r.target_kind;
        bool v_ok = k_ok;
        if (k_ok && (r.target_kind == "process_name" || r.target_kind == "pid")) v_ok = norm(f.target.value) == norm(r.target_value);
        if (k_ok && r.target_kind == "reference") v_ok = f.target.ordinal == ordinal_of(r.target_value);
        const bool c_ok = r.concept_key.empty() || r.concept_key == f.concept_key;
        const bool a_ok = r.action.empty() || r.action == intent::to_string(f.action);
        const bool e2e = i_ok && v_ok && c_ok && a_ok;
        ok_intent += i_ok;
        ok_kind += k_ok;
        ok_target += v_ok;
        ok_e2e += e2e;
        if (i_ok && !v_ok) ++intent_ok_but_wrong_target;
        auto& pr = per_intent[r.intent];
        pr.second++;
        pr.first += e2e;
        if (!i_ok) confusions[r.intent + " -> " + pi]++;
        if (!e2e && err.is_open()) {
            err << r.id << '\t' << r.text << '\t' << r.intent << '\t' << pi << '\t' << r.target_kind << ':' << r.target_value
                << '\t' << pk << ':' << f.target.value << '\n';
        }
    }
    std::sort(lat_us.begin(), lat_us.end());
    auto q = [&](double x) { return lat_us[std::min(lat_us.size() - 1, static_cast<std::size_t>(x * static_cast<double>(lat_us.size())))]; };
    double mean = 0;
    for (double v : lat_us) mean += v;
    mean /= static_cast<double>(lat_us.size());

    json::Writer w;
    w.begin_object().kv("data", data).kv("parser", parser).kv("n", n);
    if (model) w.kv("model_params", model->parameter_count()).kv("model_bytes", model->weight_bytes()).kv("arch", model->arch());
    w.kv("intent_acc", pct(ok_intent, n)).kv("target_kind_acc", pct(ok_kind, n)).kv("target_acc", pct(ok_target, n))
        .kv("e2e_acc", pct(ok_e2e, n)).kv("schema_valid", pct(valid, n))
        .kv("intent_ok_target_wrong", intent_ok_but_wrong_target)
        .kv("latency_us_mean", mean).kv("latency_us_p50", q(0.5)).kv("latency_us_p99", q(0.99));
    w.key("per_intent_e2e").begin_object();
    for (const auto& [k, v] : per_intent) w.kv(k, pct(v.first, v.second));
    w.end_object();
    std::vector<std::pair<std::size_t, std::string>> conf;
    for (const auto& [k, v] : confusions) conf.emplace_back(v, k);
    std::sort(conf.rbegin(), conf.rend());
    w.key("top_confusions").begin_array();
    for (std::size_t i = 0; i < conf.size() && i < 10; ++i) w.value(conf[i].second + " x" + std::to_string(conf[i].first));
    w.end_array();
    w.end_object();
    if (as_json) {
        std::puts(w.str().c_str());
    } else {
        std::printf("%-8s n=%zu intent=%.1f%% target=%.1f%% e2e=%.1f%% valid=%.1f%% (intent-ok/target-wrong=%zu) "
                    "latency mean=%.1fus p99=%.1fus\n",
                    parser.c_str(), n, pct(ok_intent, n), pct(ok_target, n), pct(ok_e2e, n), pct(valid, n),
                    intent_ok_but_wrong_target, mean, q(0.99));
    }
    return 0;
}
