// SPDX-License-Identifier: Apache-2.0
// Inference path: model-file validation and a hand-constructed model whose
// outputs can be computed by hand.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "osie/inference/neural_parser.hpp"

using namespace osie;
using namespace osie::inference;

namespace {

void put_f32(std::string& s, std::initializer_list<float> v) {
    for (float f : v) s.append(reinterpret_cast<const char*>(&f), 4);
}

/// bag model: buckets=4, dim=2, hidden=2, intents={help,unsupported}, tags={O,B-PROC}.
/// Embedding rows are all (1,1) so every token embeds to (1,1); proj = identity.
std::string tiny_bag(bool int8_emb, const std::string& extra_header = "") {
    std::string h = "OSIEM1\narch bag\nbuckets 4\ndim 2\nhidden 2\nngram 2 2\n" + extra_header +
                    "intents help unsupported\ntags O B-PROC\n";
    h += std::string("tensor emb ") + (int8_emb ? "i8" : "f32") + " 4 2\n";
    h += "tensor proj_w f32 2 2\ntensor proj_b f32 2\n";
    h += "tensor intent_w f32 2 4\ntensor intent_b f32 2\ntensor tag_w f32 2 2\ntensor tag_b f32 2\ndata\n";
    std::string d;
    if (int8_emb) {
        put_f32(d, {1.0F / 127, 1.0F / 127, 1.0F / 127, 1.0F / 127});  // per-row scales
        for (int i = 0; i < 8; ++i) d.push_back(static_cast<char>(127));
        while ((d.size() % 4) != 0) d.push_back('\0');
    } else {
        put_f32(d, {1, 1, 1, 1, 1, 1, 1, 1});
    }
    put_f32(d, {1, 0, 0, 1});         // proj_w = I
    put_f32(d, {0, 0});               // proj_b
    put_f32(d, {1, 0, 0, 0, 0, 0, 0, 0});  // intent 0 reads mean[0]
    put_f32(d, {0, 0});
    put_f32(d, {0, 0, 1, 0});         // tag 1 (B-PROC) reads h[0]
    put_f32(d, {0, 0});
    return h + d;
}

}  // namespace

TEST(Inference, Fnv1aKnownVectors) {
    EXPECT_EQ(fnv1a(""), 2166136261U);
    EXPECT_EQ(fnv1a("a"), 0xe40c292cU);
    EXPECT_EQ(fnv1a("foobar"), 0xbf9cf968U);
}

TEST(Inference, TokenFeaturesStructure) {
    auto f = token_features("Chrome", 1 << 20, 2, 4);
    // 1 word + n-grams of "<chrome>" (7 bigrams, 6 trigrams, 5 four-grams) + 1 shape
    EXPECT_EQ(f.size(), 1U + 7 + 6 + 5 + 1);
    auto k = token_features("cpu", 1 << 20, 2, 4, true);
    EXPECT_EQ(k.size(), token_features("cpu", 1 << 20, 2, 4, false).size() + 1);
    EXPECT_EQ(knowledge_category_list("swap"), "mem");
    EXPECT_TRUE(knowledge_category_list("banana").empty());
}

TEST(Inference, HandComputedForward) {
    auto m = IntentModel::load_from_memory(tiny_bag(false));
    ASSERT_TRUE(m.ok()) << m.error().describe();
    auto toks = intent::tokenize("hi there");
    auto out = m->forward(toks);
    ASSERT_EQ(out.intent_probs.size(), 2U);
    // logits = (1, 0) -> softmax
    const double e = std::exp(1.0);
    EXPECT_NEAR(out.intent_probs[0], e / (e + 1.0), 1e-6);
    ASSERT_EQ(out.tag_probs.size(), 2U);
    EXPECT_NEAR(out.tag_probs[0][1], e / (e + 1.0), 1e-6);
    auto p = m->parse(toks);
    EXPECT_EQ(p.intent, intent::Intent::help);
    ASSERT_EQ(p.spans.size(), 2U) << "B-PROC B-PROC: each B tag starts a new span";
    EXPECT_EQ(m->parameter_count(), 8U + 4 + 2 + 8 + 2 + 4 + 2);
}

TEST(Inference, Int8EmbeddingWithUnalignedHeader) {
    // Regression: int8 padding must be relative to the data section, not the file.
    for (const std::string pad : {"", "x 1\n", "xx 1\n", "xxx 1\n"}) {
        auto m = IntentModel::load_from_memory(tiny_bag(true, pad));
        ASSERT_TRUE(m.ok()) << "header pad '" << pad << "': " << m.error().describe();
        auto out = m->forward(intent::tokenize("hi"));
        const double e = std::exp(1.0);
        EXPECT_NEAR(out.intent_probs[0], e / (e + 1.0), 1e-5);
    }
}

TEST(Inference, RejectsMalformedFiles) {
    EXPECT_FALSE(IntentModel::load_from_memory("NOTOSIEM\n").ok());
    auto good = tiny_bag(false);
    EXPECT_FALSE(IntentModel::load_from_memory(good.substr(0, good.size() - 5)).ok()) << "truncated";
    std::string bad_shape = good;
    bad_shape.replace(bad_shape.find("tensor proj_b f32 2"), 19, "tensor proj_b f32 3");
    EXPECT_FALSE(IntentModel::load_from_memory(bad_shape).ok()) << "shape mismatch";
    std::string bad_arch = good;
    bad_arch.replace(bad_arch.find("arch bag"), 8, "arch xyz");
    EXPECT_FALSE(IntentModel::load_from_memory(bad_arch).ok());
    EXPECT_FALSE(IntentModel::load("/nonexistent/model.osiem").ok());
}

TEST(Inference, ShippedModelLoadsIfPresent) {
    const std::string path = std::string(OSIE_SOURCE_DIR) + "/models/intent.osiem";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "no trained model in models/";
    auto m = IntentModel::load(path);
    ASSERT_TRUE(m.ok()) << m.error().describe();
    HybridParser hp(std::make_shared<IntentModel>(std::move(m).value()));
    auto p = hp.parse(intent::tokenize("why is my mac so slow"));
    EXPECT_EQ(p.intent, intent::Intent::diagnose_slow);
}

// ------------------------------------------------- loader hardening (P0) ---

namespace {

std::string with_line_replaced(std::string s, const std::string& from, const std::string& to) {
    const auto at = s.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

std::string reason_of(const std::string& bytes) {
    auto m = IntentModel::load_from_memory(bytes);
    return m.ok() ? std::string("accepted") : m.error().message;
}

}  // namespace

TEST(InferenceHardening, ShapeProductOverflowIsRejectedBeforeAllocation) {
    // Regression: 2^26 * 2^26 * 2^10 elements wrapped the old size check to 0
    // bytes and then aborted the process with std::length_error.
    const std::string h = "OSIEM1\narch bag\nbuckets 4\ndim 2\nhidden 2\nintents help\ntags O\n"
                          "tensor emb f32 4194304 4194304 1024\ndata\n";
    EXPECT_FALSE(IntentModel::load_from_memory(h).ok());
    const std::string h2 = "OSIEM1\narch bag\nbuckets 4\ndim 2\nhidden 2\nintents help\ntags O\n"
                           "tensor emb f32 65536 65536\ndata\n";
    EXPECT_NE(reason_of(h2).find("exceeds"), std::string::npos) << reason_of(h2);
}

TEST(InferenceHardening, RejectsMalformedHeaders) {
    const auto good = tiny_bag(false);
    ASSERT_EQ(reason_of(good), "accepted");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"buckets 4\n", "buckets -4\n"},                      // stoul used to accept a sign
        {"buckets 4\n", "buckets 4294967300\n"},              // used to truncate to 4
        {"buckets 4\n", "buckets 4abc\n"},                    // trailing garbage
        {"buckets 4\n", "buckets 0\n"},
        {"dim 2\n", "dim 1000000\n"},
        {"ngram 2 2\n", "ngram 3 2\n"},
        {"ngram 2 2\n", "ngram 0 2\n"},
        {"ngram 2 2\n", "ngram 2 99\n"},
        {"buckets 4\n", "buckets 4\nbuckets 4\n"},            // duplicate key
        {"intents help unsupported\n", "intents help help\n"},
        {"intents help unsupported\n", "intents help \x1b[2Jx\n"},  // control characters
        {"intents help unsupported\n", "intents help caf\xc3\xa9\n"},  // non-ASCII label
        {"tensor proj_b f32 2\n", "tensor proj_b f32 2\ntensor proj_b f32 2\n"},  // duplicate tensor
        {"tensor proj_b f32 2\n", "tensor proj_b f32 2 1 1 1\n"},                // rank > 3
        {"tensor proj_b f32 2\n", "tensor proj_b f64 2\n"},
        {"arch bag\n", "arch bag extra\n"},
        {"data\n", "dat\n"},                                   // no data section
    };
    for (const auto& [from, to] : cases) {
        EXPECT_NE(reason_of(with_line_replaced(good, from, to)), "accepted") << "accepted: " << to;
    }
}

TEST(InferenceHardening, RejectsBadThresholdsAndWeights) {
    const auto good = tiny_bag(false);
    for (const std::string th : {"nan", "inf", "-0.1", "1.5", "0.5x", ""}) {
        EXPECT_NE(reason_of(with_line_replaced(good, "data\n", "hybrid_threshold " + th + "\ndata\n")), "accepted") << th;
    }
    EXPECT_EQ(reason_of(with_line_replaced(good, "data\n", "hybrid_threshold 0.6\ndata\n")), "accepted");

    // A NaN weight would make every softmax NaN and every decision arbitrary.
    std::string nan_w = good;
    const float nan = std::nanf("");
    std::memcpy(nan_w.data() + nan_w.size() - 4, &nan, 4);
    EXPECT_NE(reason_of(nan_w).find("non-finite"), std::string::npos);

    // Regression (found by libFuzzer): finite weights near FLT_MAX overflowed
    // to NaN probabilities in the forward pass.
    std::string huge = good;
    const float big = 3.4e38F;
    std::memcpy(huge.data() + huge.size() - 4, &big, 4);
    EXPECT_NE(reason_of(huge).find("out-of-range"), std::string::npos);
}

TEST(InferenceHardening, RejectsTrailingBytesAndExtraTensors) {
    const auto good = tiny_bag(false);
    EXPECT_NE(reason_of(good + "junk").find("trailing"), std::string::npos);
    // An extra, well-formed tensor that the architecture does not use.
    std::string extra = with_line_replaced(good, "data\n", "tensor spare f32 1\ndata\n");
    extra.append(4, '\0');
    EXPECT_NE(reason_of(extra).find("extra tensors"), std::string::npos);
}

TEST(InferenceHardening, UnknownMetadataKeysAreIgnoredForCompatibility) {
    EXPECT_EQ(reason_of(tiny_bag(false, "trained_by unit-test\n")), "accepted");
    EXPECT_NE(reason_of(tiny_bag(false, "bad\x01key 1\n")), "accepted");
}

TEST(InferenceHardening, OversizedFileIsRejectedWithoutReadingIt) {
    const auto path = std::filesystem::temp_directory_path() / "osie_sparse_model.osiem";
    {
        std::ofstream f(path, std::ios::binary);
        f << "OSIEM1\n";
    }
    std::filesystem::resize_file(path, limits::max_file_bytes + 1);  // sparse: no disk or RAM cost
    auto m = IntentModel::load(path.string());
    std::filesystem::remove(path);
    ASSERT_FALSE(m.ok());
    EXPECT_NE(m.error().message.find("exceeds"), std::string::npos) << m.error().message;
}

TEST(InferenceHardening, RandomMutationsNeverCrash) {
    // Deterministic mutation fuzzing of valid f32 and int8 models: byte flips,
    // digit edits in the header and truncation. Every input must either be
    // rejected or produce finite probabilities. Run under ASan/UBSan in CI;
    // tests/fuzz/ holds the libFuzzer harness for open-ended fuzzing.
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    auto rnd = [&state](std::uint64_t n) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % n;
    };
    const auto toks = intent::tokenize("why is chrome using so much cpu");
    std::size_t accepted = 0;
    for (const bool int8 : {false, true}) {
        const auto base = tiny_bag(int8, "hybrid_threshold 0.5\n");
        const auto header_end = base.find("data\n");
        for (int iter = 0; iter < 4000; ++iter) {
            std::string s = base;
            const auto edits = 1 + rnd(4);
            for (std::uint64_t e = 0; e < edits && !s.empty(); ++e) {
                const std::size_t header = std::min(header_end, s.size());
                switch (rnd(4)) {
                    case 0: s[rnd(s.size())] = static_cast<char>(rnd(256)); break;
                    case 1: s[rnd(header)] = static_cast<char>('0' + rnd(10)); break;
                    case 2: s.resize(rnd(s.size() + 1)); break;
                    default: s.insert(rnd(header + 1), std::string(1 + rnd(3), static_cast<char>('0' + rnd(10)))); break;
                }
            }
            auto m = IntentModel::load_from_memory(s);
            if (!m) continue;
            ++accepted;
            const auto out = m->forward(toks);
            for (float p : out.intent_probs) ASSERT_TRUE(std::isfinite(p));
            (void)m->parse(toks);
        }
    }
    EXPECT_GT(accepted, 0U) << "mutations should sometimes leave a valid model (e.g. weight-only flips)";
}
