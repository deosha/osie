// SPDX-License-Identifier: Apache-2.0
// libFuzzer harness for the .osiem loader (docs/MODEL_FORMAT.md).
// Build: cmake -DOSIE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ (Linux clang with
// libclang-rt). Seeds: models/*.osiem. Any accepted model must run a forward
// pass with finite outputs.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "osie/inference/neural_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using osie::inference::IntentModel;
    auto m = IntentModel::load_from_memory(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!m) return 0;
    static const auto toks = osie::intent::tokenize("why is Google Chrome Helper using so much cpu pid 42");
    const auto out = m->forward(toks);
    for (float p : out.intent_probs)
        if (!std::isfinite(p)) std::abort();
    (void)m->parse(toks);
    return 0;
}
