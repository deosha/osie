// SPDX-License-Identifier: Apache-2.0
// Minimal specialised inference for OSIE's tiny intent models.
//
// Rationale (see docs/RESEARCH.md): the models are < 3M parameters and use
// three operators (embedding-bag, GRU/conv, linear). A dependency-free
// forward pass is ~300 lines, loads in milliseconds and needs no runtime.
// Measured against ONNX Runtime 1.31 on identical weights (docs/BENCHMARKS.md):
// 1-6x lower latency, 4-8x faster load, ~6x less memory.
//
// File format (.osiem): text header lines ("key values..."), a line "data",
// then little-endian tensors in header order. f32 tensors are raw floats;
// i8 tensors are per-row f32 scales followed by int8 values.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "osie/core/result.hpp"
#include "osie/intent/intent.hpp"

namespace osie::inference {

/// 32-bit FNV-1a over bytes. Mirrored in ml/osie_ml/features.py.
constexpr std::uint32_t fnv1a(std::string_view s) noexcept {
    std::uint32_t h = 2166136261U;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619U;
    }
    return h;
}

/// Hashed feature ids for one token (word, char n-grams, shape).
std::vector<std::uint32_t> token_features(std::string_view token_text, std::uint32_t buckets, int ngram_min,
                                          int ngram_max, bool knowledge = false);
/// OS-domain lexicon category of a lower-cased token ("" if none).
std::string_view knowledge_category_list(std::string_view lower_token);

/// Hard limits for .osiem files (docs/MODEL_FORMAT.md). Generous relative to
/// the shipped models (<= 9.6M parameters), small enough that a malformed
/// header cannot trigger a large allocation.
namespace limits {
inline constexpr std::uint64_t max_file_bytes = 256ULL << 20;  // 256 MiB
inline constexpr std::size_t max_header_bytes = 64U << 10;
inline constexpr std::size_t max_header_lines = 256;
inline constexpr std::size_t max_tensors = 32;
inline constexpr std::size_t max_rank = 3;
inline constexpr std::uint64_t max_dim = 1U << 22;
inline constexpr std::size_t max_tensor_elements = std::size_t{1} << 26;  // 256 MiB as f32
inline constexpr std::uint64_t max_buckets = 1U << 22;
inline constexpr std::uint64_t max_width = 4096;  // dim, hidden
inline constexpr std::uint64_t max_ngram = 8;
inline constexpr std::size_t max_intents = 128;
inline constexpr std::size_t max_tags = 64;
inline constexpr std::size_t max_label_chars = 64;
inline constexpr float max_abs_weight = 1e6F;  // every f32 weight and int8 row scale
}  // namespace limits

struct Tensor {
    std::vector<std::size_t> shape;
    std::vector<float> f32;         // dense float data (or dequantised scales when i8)
    std::vector<std::int8_t> i8;    // quantised rows (embedding only)
    std::vector<float> row_scale;   // per-row scale for i8
    [[nodiscard]] bool quantized() const noexcept { return !i8.empty(); }
    [[nodiscard]] std::size_t rows() const { return shape.empty() ? 0 : shape[0]; }
    [[nodiscard]] std::size_t cols() const { return shape.size() < 2 ? 1 : shape[1]; }
};

struct ModelOutput {
    std::vector<float> intent_probs;          // softmax over intents
    std::vector<std::vector<float>> tag_probs; // per token, softmax over tags
};

class IntentModel {
public:
    static Result<IntentModel> load(const std::string& path);
    static Result<IntentModel> load_from_memory(std::string_view bytes);

    [[nodiscard]] ModelOutput forward(const std::vector<intent::Token>& toks) const;
    /// Greedy decode with BIO repair -> ParseOutput.
    [[nodiscard]] intent::ParseOutput parse(const std::vector<intent::Token>& toks) const;

    [[nodiscard]] const std::string& arch() const noexcept { return arch_; }
    [[nodiscard]] std::uint32_t buckets() const noexcept { return buckets_; }
    [[nodiscard]] int ngram_min() const noexcept { return ngram_min_; }
    [[nodiscard]] int ngram_max() const noexcept { return ngram_max_; }
    [[nodiscard]] bool knowledge() const noexcept { return knowledge_; }
    /// Confidence below which HybridParser defers to the rules (calibrated on dev).
    [[nodiscard]] std::optional<float> hybrid_threshold() const noexcept { return hybrid_threshold_; }
    [[nodiscard]] std::size_t parameter_count() const;
    [[nodiscard]] std::size_t weight_bytes() const;
    [[nodiscard]] const std::vector<std::string>& intent_labels() const noexcept { return intents_; }
    [[nodiscard]] const std::vector<std::string>& tag_labels() const noexcept { return tags_; }

private:
    static Result<IntentModel> parse_model(std::string_view bytes);
    const Tensor& t(const std::string& name) const;
    void embed(const std::vector<intent::Token>& toks, std::vector<float>& x) const;  // [T, D]

    std::string arch_;  // "bigru" | "cnn" | "bag"
    std::uint32_t buckets_{0};
    std::size_t dim_{0}, hidden_{0};
    int ngram_min_{2}, ngram_max_{4};
    bool knowledge_{false};
    std::optional<float> hybrid_threshold_;
    std::vector<std::string> intents_, tags_;
    std::vector<int> intent_map_;  // model label index -> intent::Intent (or -1)
    std::unordered_map<std::string, Tensor> tensors_;
};

/// Pure neural parser: intent and spans from the model only.
class NeuralParser final : public intent::IIntentParser {
public:
    explicit NeuralParser(std::shared_ptr<const IntentModel> m) : m_(std::move(m)) {}
    intent::ParseOutput parse(const std::vector<intent::Token>& toks) const override { return m_->parse(toks); }
    [[nodiscard]] std::string_view name() const noexcept override { return "neural"; }

private:
    std::shared_ptr<const IntentModel> m_;
};

/// Neural first; falls back to the deterministic rules when the model's
/// confidence is below `threshold`. Slots: model spans, completed by rule
/// spans when the model found no target for a process intent.
class HybridParser final : public intent::IIntentParser {
public:
    explicit HybridParser(std::shared_ptr<const IntentModel> m, std::optional<float> threshold = std::nullopt)
        : m_(std::move(m)), threshold_(threshold.value_or(m_->hybrid_threshold().value_or(0.55F))) {}
    intent::ParseOutput parse(const std::vector<intent::Token>& toks) const override;
    [[nodiscard]] std::string_view name() const noexcept override { return "hybrid"; }

private:
    std::shared_ptr<const IntentModel> m_;
    intent::RuleParser rules_;
    float threshold_;
};

enum class ParserChoice : std::uint8_t { rules, neural, hybrid };

/// The parser the CLI and the agent API answer with. `rules` needs no model.
/// Otherwise the intent model is located (an explicit path, $OSIE_MODEL, then
/// the installed and build locations); if none is found, or a model that was
/// not explicitly requested fails to load, the rules are used and `note` says
/// why. A model the caller asked for that fails to load is an error: returns
/// nullptr with `error` set, since answering with another parser would
/// misreport what ran.
std::unique_ptr<intent::IIntentParser> make_parser(ParserChoice choice, const std::optional<std::string>& model_path,
                                                   std::string& note, std::string& error);

}  // namespace osie::inference
