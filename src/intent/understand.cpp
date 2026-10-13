// SPDX-License-Identifier: Apache-2.0
// The staged language-understanding pipeline shared by the engine and the
// evaluators: normalise -> safety assessment -> parser -> frame -> validation.
#include "osie/intent/intent.hpp"

namespace osie::intent {

IntentFrame understand(std::string_view text, const IIntentParser& parser, Platform platform,
                       const ObservationSet& supported, const CapabilityRegistry& reg, std::vector<Token>* toks_out) {
    auto toks = tokenize(text);
    normalize(toks);
    // Safety is assessed before, and independently of, the parser.
    const auto safety = assess_safety(text, toks);
    auto p = parser.parse(toks);
    // Parser output is untrusted (it may come from a model file).
    if (static_cast<std::size_t>(p.intent) >= kIntentCount) {
        p.intent = Intent::unsupported;
        p.spans.clear();
        p.parser += "+sanitized";
    }
    if (!(p.confidence >= 0.0F && p.confidence <= 1.0F)) {
        p.confidence = 0.0F;
        p.parser += "+sanitized";
    }
    p.safety = safety;
    auto f = build_frame(p, toks, platform, supported, reg);
    f.validation_errors = validate(f);
    if (toks_out) *toks_out = std::move(toks);
    return f;
}

}  // namespace osie::intent
