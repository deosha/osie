// SPDX-License-Identifier: Apache-2.0
// Inference path: model-file validation and a hand-constructed model whose
// outputs can be computed by hand.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>

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
