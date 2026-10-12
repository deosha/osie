// SPDX-License-Identifier: Apache-2.0
// Google Benchmark harness: native observation costs, graph/reasoning costs,
// intent parsing (rules vs. neural), and end-to-end question latency.
#include <benchmark/benchmark.h>
#include <sys/resource.h>

#include <filesystem>

#include "osie/architecture/model.hpp"
#include "osie/engine/engine.hpp"
#include "osie/inference/neural_parser.hpp"
#include "osie/reasoning/reasoner.hpp"

using namespace osie;

namespace {

const char* kUtterances[] = {
    "Why is my Mac using so much CPU?",
    "which app is using the most memory right now",
    "why is google chrome helper using so much ram",
    "tell me about pid 4242",
    "what happens if I change this CPU limit?",
};

std::shared_ptr<inference::IntentModel> load_model(const std::string& name) {
    const std::string path = std::string(OSIE_SOURCE_DIR) + "/models/" + name;
    if (!std::filesystem::exists(path)) return nullptr;
    auto m = inference::IntentModel::load(path);
    return m ? std::make_shared<inference::IntentModel>(std::move(m).value()) : nullptr;
}

SystemState real_state(IOperatingSystem& os) {
    auto reg = CapabilityRegistry::builtin();
    SafetyPolicy pol;
    AuditLog audit;
    ObservationBroker b(os, reg, pol, audit);
    StateSampler s(b);
    std::vector<ObservationKind> all = os.supported().list();
    return s.sample(SampleRequest{all, Millis{100}});
}

}  // namespace

// ---------------------------------------------------------------- native ---
static void BM_Native_CpuSample(benchmark::State& st) {
    auto os = make_native_os();
    for (auto _ : st) benchmark::DoNotOptimize(os->cpu_sample());
}
BENCHMARK(BM_Native_CpuSample);

static void BM_Native_Memory(benchmark::State& st) {
    auto os = make_native_os();
    for (auto _ : st) benchmark::DoNotOptimize(os->memory());
}
BENCHMARK(BM_Native_Memory);

static void BM_Native_ProcessSnapshot(benchmark::State& st) {
    auto os = make_native_os();
    std::size_t n = 0;
    for (auto _ : st) {
        auto r = os->processes(ProcessQuery{true, static_cast<bool>(st.range(0))}, {});
        n = r ? r->value.processes.size() : 0;
        benchmark::DoNotOptimize(r);
    }
    st.counters["processes"] = static_cast<double>(n);
}
BENCHMARK(BM_Native_ProcessSnapshot)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

static void BM_Native_Filesystems(benchmark::State& st) {
    auto os = make_native_os();
    for (auto _ : st) benchmark::DoNotOptimize(os->filesystems());
}
BENCHMARK(BM_Native_Filesystems);

static void BM_Native_Network(benchmark::State& st) {
    auto os = make_native_os();
    for (auto _ : st) benchmark::DoNotOptimize(os->network_interfaces());
}
BENCHMARK(BM_Native_Network);

// ------------------------------------------------------- graph/reasoning ---
static void BM_Graph_LoadOntology(benchmark::State& st) {
    for (auto _ : st) {
        arch::KnowledgeGraph g;
        arch::load_ontology(g);
        benchmark::DoNotOptimize(g.stats());
    }
}
BENCHMARK(BM_Graph_LoadOntology);

static void BM_Graph_IncrementalUpdate(benchmark::State& st) {
    auto os = make_native_os();
    const auto s = real_state(*os);
    arch::KnowledgeGraph g;
    arch::load_ontology(g);
    arch::update_instances(g, s, os->platform());
    for (auto _ : st) benchmark::DoNotOptimize(arch::update_instances(g, s, os->platform()));
    st.counters["nodes"] = static_cast<double>(g.stats().nodes_alive);
    st.counters["edges"] = static_cast<double>(g.stats().edges_alive);
}
BENCHMARK(BM_Graph_IncrementalUpdate)->Unit(benchmark::kMicrosecond);

static void BM_Graph_Consistency(benchmark::State& st) {
    auto os = make_native_os();
    const auto s = real_state(*os);
    arch::KnowledgeGraph g;
    arch::load_ontology(g);
    arch::update_instances(g, s, os->platform());
    for (auto _ : st) benchmark::DoNotOptimize(g.check_consistency());
}
BENCHMARK(BM_Graph_Consistency)->Unit(benchmark::kMicrosecond);

static void BM_Reason_Slowness(benchmark::State& st) {
    auto os = make_native_os();
    const auto s = real_state(*os);
    arch::KnowledgeGraph g;
    arch::load_ontology(g);
    reason::Reasoner r(g, os->platform());
    for (auto _ : st) benchmark::DoNotOptimize(r.slowness(s));
}
BENCHMARK(BM_Reason_Slowness)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------- intent ---
static void BM_Intent_Tokenize(benchmark::State& st) {
    std::size_t i = 0;
    for (auto _ : st) benchmark::DoNotOptimize(intent::tokenize(kUtterances[i++ % 5]));
}
BENCHMARK(BM_Intent_Tokenize);

static void BM_Intent_Rules(benchmark::State& st) {
    intent::RuleParser p;
    std::vector<std::vector<intent::Token>> toks;
    for (auto* u : kUtterances) toks.push_back(intent::tokenize(u));
    std::size_t i = 0;
    for (auto _ : st) benchmark::DoNotOptimize(p.parse(toks[i++ % toks.size()]));
}
BENCHMARK(BM_Intent_Rules);

static void BM_Intent_Neural(benchmark::State& st, const char* file) {
    auto m = load_model(file);
    if (!m) {
        st.SkipWithError("model file not found (run ml/experiments/run_all.py)");
        return;
    }
    std::vector<std::vector<intent::Token>> toks;
    for (auto* u : kUtterances) toks.push_back(intent::tokenize(u));
    std::size_t i = 0;
    for (auto _ : st) benchmark::DoNotOptimize(m->parse(toks[i++ % toks.size()]));
    st.counters["params"] = static_cast<double>(m->parameter_count());
}
BENCHMARK_CAPTURE(BM_Intent_Neural, bag, "intent_bag.osiem");
BENCHMARK_CAPTURE(BM_Intent_Neural, cnn, "intent_cnn.osiem");
BENCHMARK_CAPTURE(BM_Intent_Neural, gru, "intent_gru.osiem");
BENCHMARK_CAPTURE(BM_Intent_Neural, gru_int8, "intent_gru.int8.osiem");
BENCHMARK_CAPTURE(BM_Intent_Neural, deployed, "intent.osiem");
BENCHMARK_CAPTURE(BM_Intent_Neural, deployed_int8, "intent.int8.osiem");

static void BM_Intent_ModelLoad(benchmark::State& st) {
    const std::string path = std::string(OSIE_SOURCE_DIR) + "/models/intent.osiem";
    if (!std::filesystem::exists(path)) {
        st.SkipWithError("model not found");
        return;
    }
    for (auto _ : st) benchmark::DoNotOptimize(inference::IntentModel::load(path));
}
BENCHMARK(BM_Intent_ModelLoad)->Unit(benchmark::kMillisecond);

// ------------------------------------------------------------ end to end ---
static void BM_Engine_AskCached(benchmark::State& st) {
    EngineConfig cfg;
    cfg.sample_interval = Millis{100};
    cfg.freshness_budget = Millis{3'600'000};  // measure everything except re-sampling
    Engine e(make_native_os(), std::make_unique<intent::RuleParser>(), cfg);
    e.ask("why is my computer slow");
    for (auto _ : st) benchmark::DoNotOptimize(e.ask("why is my computer slow"));
}
BENCHMARK(BM_Engine_AskCached)->Unit(benchmark::kMicrosecond);

static void BM_Engine_AskFresh(benchmark::State& st) {
    EngineConfig cfg;
    cfg.sample_interval = Millis{0};  // two back-to-back samples: pure observation cost
    cfg.freshness_budget = Millis{0};
    Engine e(make_native_os(), std::make_unique<intent::RuleParser>(), cfg);
    for (auto _ : st) benchmark::DoNotOptimize(e.ask("why is my computer slow"));
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    st.counters["peak_rss_mb"] = static_cast<double>(ru.ru_maxrss) / 1048576.0;
#else
    st.counters["peak_rss_mb"] = static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}
BENCHMARK(BM_Engine_AskFresh)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
