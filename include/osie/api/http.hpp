// SPDX-License-Identifier: Apache-2.0
// HTTP/1.x request framing and policy for the local API (docs/API.md).
//
// Deliberately narrow: one request per connection (the response always
// carries "Connection: close"), bodies framed by Content-Length only, no
// chunked transfer coding, no pipelining, no keep-alive. The request line and
// headers are tokenised by picohttpparser (third_party/); everything here is
// policy on top of it. Pure functions: no sockets, so they are unit-tested
// and fuzzed directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osie::api {

/// Stable error categories (the `code` of every error response).
enum class ErrorCode : std::uint8_t {
    invalid_request,
    unauthorized,
    forbidden,
    unsupported,
    insufficient_evidence,
    stale_evidence,
    rate_limited,
    busy,
    internal_error,
};
std::string_view to_string(ErrorCode c) noexcept;

struct ApiError {
    int status{400};  // HTTP status
    ErrorCode code{ErrorCode::invalid_request};
    std::string reason;   // machine-readable detail, e.g. "body_too_large"
    std::string message;  // one human sentence; never a path, a stack or a secret
    bool retryable{false};
    std::optional<int> retry_after_s;
};

struct HttpRequest {
    std::string method;
    std::string path;   // target before '?'
    std::string query;  // after '?', may be empty
    int minor_version{1};
    std::optional<std::string> content_type;  // media type, lowercased, parameters removed
    std::optional<std::size_t> content_length;
    bool expect_continue{false};
    std::string request_id;  // X-Request-Id, if present and well-formed
    std::size_t head_bytes{0};  // bytes of the request line and headers
    std::string body;
};

struct HeadLimits {
    std::size_t max_head_bytes{8 * 1024};
    std::size_t max_headers{32};
    std::size_t max_body_bytes{16 * 1024};
    std::size_t max_target_bytes{512};
};

/// Result of parsing the bytes received so far.
struct HeadParse {
    enum class State : std::uint8_t { incomplete, complete, error } state{State::incomplete};
    HttpRequest request;  // complete: request line and headers (no body yet)
    ApiError error;       // error: the response to send
};

/// Parses the request line and headers from the start of `buf`. Policy:
/// HTTP/1.0 or 1.1; a target of printable ASCII without fragments; no
/// obsolete line folding; at most one Content-Length, Content-Type, Host and
/// Expect; Transfer-Encoding is refused (send Content-Length); a body larger
/// than `max_body_bytes` is refused before it is read.
HeadParse parse_head(std::string_view buf, const HeadLimits& lim);

/// Query string "a=1&b=x": at most `max_params` pairs, names and values of
/// [A-Za-z0-9_.,-] only (no percent-encoding), no duplicates, no empty names.
std::optional<std::map<std::string, std::string>> parse_query(std::string_view q, std::size_t max_params = 8);

struct HttpResponse {
    int status{200};
    std::string body;  // JSON
    std::vector<std::pair<std::string, std::string>> headers;  // extra headers
    // Measurements for the server's statistics (never sent): time spent
    // waiting for the engine and inside it, for diagnoses; -1 otherwise.
    double engine_wait_ms{-1}, engine_ms{-1};
};

/// Serialises a response: status line, Content-Type: application/json,
/// Content-Length, Cache-Control: no-store, Connection: close, extra headers.
std::string serialize(const HttpResponse& r);

std::string_view status_text(int status) noexcept;

}  // namespace osie::api
