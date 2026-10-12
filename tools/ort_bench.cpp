// SPDX-License-Identifier: Apache-2.0
// Benchmark: OSIE custom C++ inference vs ONNX Runtime (C++ API) on the same
// weights and the same hashed features. Single-utterance, single-thread.
//
//   osie_ort_bench MODEL.osiem MODEL.onnx DATA.tsv
#include <onnxruntime_cxx_api.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "osie/inference/neural_parser.hpp"

using namespace osie;
using clk = std::chrono::steady_clock;

static long rss_kb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: osie_ort_bench MODEL.osiem MODEL.onnx DATA.tsv\n");
        return 2;
    }
    std::vector<std::string> texts;
    {
        std::ifstream in(argv[3]);
        std::string line;
        std::getline(in, line);  // header
        while (std::getline(in, line) && texts.size() < 500) {
            auto a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
            texts.push_back(line.substr(b + 1, c - b - 1));
        }
    }
    const long rss0 = rss_kb();
    auto t0 = clk::now();
    auto model = inference::IntentModel::load(argv[1]);
    const double load_custom_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    if (!model) {
        std::fprintf(stderr, "%s\n", model.error().describe().c_str());
        return 1;
    }
    const long rss1 = rss_kb();

    t0 = clk::now();
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "osie");
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(1);
    so.SetInterOpNumThreads(1);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session(env, argv[2], so);
    const double load_ort_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    const long rss2 = rss_kb();
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const char* in_names[] = {"ids", "offsets"};
    const char* out_names[] = {"intent", "tags"};


    double max_diff = 0;
    std::size_t agree = 0, n = 0;
    std::vector<double> t_custom, t_ort, t_feat;
    for (int rep = 0; rep < 5; ++rep) {
        for (const auto& text : texts) {
            auto toks = intent::tokenize(text);
            if (toks.empty()) continue;
            auto a = clk::now();
            auto ref = model->forward(toks);
            auto b = clk::now();
            // Same featurisation as the custom path (hashing is part of both pipelines).
            std::vector<std::int64_t> ids, offs;
            for (const auto& t : toks) {
                offs.push_back(static_cast<std::int64_t>(ids.size()));
                for (auto f : inference::token_features(t.text, model->buckets(), model->ngram_min(), model->ngram_max(), model->knowledge()))
                    ids.push_back(f);
            }
            auto c = clk::now();
            std::int64_t ishape[] = {static_cast<std::int64_t>(ids.size())};
            std::int64_t oshape[] = {static_cast<std::int64_t>(offs.size())};
            Ort::Value inputs[] = {Ort::Value::CreateTensor<std::int64_t>(mem, ids.data(), ids.size(), ishape, 1),
                                   Ort::Value::CreateTensor<std::int64_t>(mem, offs.data(), offs.size(), oshape, 1)};
            auto outs = session.Run(Ort::RunOptions{nullptr}, in_names, inputs, 2, out_names, 2);
            auto d = clk::now();
            if (rep == 0) {
                const float* ip = outs[0].GetTensorData<float>();
                std::size_t best_o = 0, best_c = 0;
                for (std::size_t i = 0; i < ref.intent_probs.size(); ++i) {
                    max_diff = std::max(max_diff, static_cast<double>(std::fabs(ip[i] - ref.intent_probs[i])));
                    if (ip[i] > ip[best_o]) best_o = i;
                    if (ref.intent_probs[i] > ref.intent_probs[best_c]) best_c = i;
                }
                agree += best_o == best_c;
                ++n;
            } else {
                t_custom.push_back(std::chrono::duration<double, std::micro>(b - a).count());
                t_feat.push_back(std::chrono::duration<double, std::micro>(c - b).count());
                t_ort.push_back(std::chrono::duration<double, std::micro>(d - c).count());
            }
        }
    }
    auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double m = 0;
        for (double x : v) m += x;
        return std::make_pair(m / static_cast<double>(v.size()), v[v.size() * 99 / 100]);
    };
    auto [cm, cp] = stats(t_custom);
    auto [om, op] = stats(t_ort);
    auto [fm, fp] = stats(t_feat);
    std::printf("{\"arch\":\"%s\",\"params\":%zu,\"n\":%zu,\"parity_max_abs_diff\":%.3g,\"argmax_agreement\":%.4f,"
                "\"custom_us_mean\":%.2f,\"custom_us_p99\":%.2f,\"ort_us_mean\":%.2f,\"ort_us_p99\":%.2f,"
                "\"featurize_us_mean\":%.2f,\"load_custom_ms\":%.2f,\"load_ort_ms\":%.2f,"
                "\"rss_delta_custom_kb\":%ld,\"rss_delta_ort_kb\":%ld}\n",
                model->arch().c_str(), model->parameter_count(), n, max_diff,
                static_cast<double>(agree) / static_cast<double>(n), cm, cp, om + fm, op + fp, fm, load_custom_ms,
                load_ort_ms, rss1 - rss0, rss2 - rss1);
    return 0;
}
