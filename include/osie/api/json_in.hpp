// SPDX-License-Identifier: Apache-2.0
// Strict parser for the API's request bodies: one JSON object whose values
// are strings, integers, booleans or null. Nested objects and arrays,
// fractional numbers, duplicate keys, invalid UTF-8 and control characters
// in strings are rejected. Small on purpose: it accepts exactly what the API
// needs and nothing else, and is fuzzed (tests/fuzz/fuzz_api_request.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>

#include "osie/core/result.hpp"

namespace osie::api {

using JsonScalar = std::variant<std::nullptr_t, bool, std::int64_t, std::string>;
using JsonObject = std::map<std::string, JsonScalar>;

struct JsonLimits {
    std::size_t max_bytes{16 * 1024};
    std::size_t max_members{16};
    std::size_t max_string_bytes{8 * 1024};
};

/// Parses `text` as one flat JSON object. The error message names what was
/// wrong (never echoes the input).
Result<JsonObject> parse_flat_object(std::string_view text, const JsonLimits& lim = {});

}  // namespace osie::api
