// SPDX-License-Identifier: Apache-2.0
// The client half of the agent's HTTP/JSON API, for `osie remote`
// (docs/API.md, "Transport"). Pure functions: no sockets, so they are
// unit-tested and fuzzed (tests/fuzz/fuzz_remote_response.cpp).
//
// The reply comes from another host, so it is untrusted input: the status
// line and headers are tokenised by picohttpparser, the body is framed by
// Content-Length only, everything is bounded, and the body must be one JSON
// object carrying "schema_version" (the mark of an agent response). The body
// is never re-serialised: --json output writes these exact bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace osie::remote {

inline constexpr std::size_t kMaxResponseBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxResponseHeadBytes = 16 * 1024;
inline constexpr std::size_t kMaxResponseHeaders = 32;
inline constexpr std::size_t kMaxJsonDepth = 64;

struct HttpReply {
    int status{0};
    std::string body;  // exact bytes received
};

/// Result of parsing the bytes received so far.
struct ReplyParse {
    enum class State : std::uint8_t { incomplete, complete, error } state{State::incomplete};
    HttpReply reply;    // complete
    std::string error;  // error: what was wrong (never echoes the input)
};

/// Parses a response from the start of `buf`. `eof` says the peer closed
/// the connection, so nothing more will arrive. Rules: HTTP/1.0 or 1.1;
/// status 200-599 (1xx is refused: OSIE never sends Expect); at most
/// kMaxResponseHeaders headers in kMaxResponseHeadBytes; exactly one
/// Content-Length of digits only; Transfer-Encoding refused; Content-Type
/// application/json; total size at most `max_bytes`; no bytes after the
/// body; the body a JSON object whose "schema_version" is "1.<digits>" (any
/// 1.x: unknown fields are ignored). Incomplete at EOF is an error (truncated).
ReplyParse parse_reply(std::string_view buf, bool eof, std::size_t max_bytes = kMaxResponseBytes);

// ------------------------------------------------------------------ JSON ---

/// A read-only JSON value. Numbers keep their exact source text.
struct JsonNode {
    enum class Kind : std::uint8_t { null, boolean, number, string, array, object } kind{Kind::null};
    bool b{false};
    std::string text;  // string: decoded value; number: source text
    std::vector<JsonNode> items;     // array elements, or object member values
    std::vector<std::string> keys;   // object: member names, parallel to items, in source order

    /// First member named `k`, or nullptr (also when this is not an object).
    [[nodiscard]] const JsonNode* get(std::string_view k) const;
    [[nodiscard]] const std::string* str(std::string_view k) const;
};

/// Strict RFC 8259 parser with a depth limit: valid UTF-8, no control
/// characters in strings, no trailing data, no duplicate keys in an object.
/// Returns nothing on any violation.
std::optional<JsonNode> parse_json(std::string_view text, std::size_t max_depth = kMaxJsonDepth);

/// Re-indents valid JSON for reading. Every string and number is copied
/// byte for byte; only whitespace between tokens changes. `text` must have
/// passed parse_json.
std::string pretty_json(std::string_view text);

// -------------------------------------------------------------- requests ---

std::string http_get(std::string_view target);
std::string http_post_json(std::string_view target, std::string_view body);

struct DiagnoseRequest {
    std::string question;
    std::string session_id;                     // empty: none
    std::optional<long> max_evidence_age_ms;
};
/// The JSON body of POST /v1/diagnose (escaped by osie::json::Writer).
std::string diagnose_body(const DiagnoseRequest& r);

/// The target of GET /v1/system/history with the given parameters, which
/// are validated syntactically here (digits; metric [a-z0-9_]{1,32}) and for
/// range by the agent. Nothing on a syntax error.
std::optional<std::string> history_target(const std::optional<std::string>& window_seconds,
                                          const std::optional<std::string>& limit,
                                          const std::optional<std::string>& metric);

}  // namespace osie::remote
