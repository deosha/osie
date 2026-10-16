// SPDX-License-Identifier: Apache-2.0
#include "osie/api/http.hpp"

#include <algorithm>
#include <cctype>

extern "C" {
#include "picohttpparser.h"
}

namespace osie::api {
namespace {

ApiError err(int status, ErrorCode code, std::string reason, std::string message) {
    ApiError e;
    e.status = status;
    e.code = code;
    e.reason = std::move(reason);
    e.message = std::move(message);
    return e;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool path_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '.';
}

bool id_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == ':' || c == '-';
}

bool param_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ',' || c == '-';
}

}  // namespace

std::string_view to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::invalid_request: return "invalid_request";
        case ErrorCode::unauthorized: return "unauthorized";
        case ErrorCode::forbidden: return "forbidden";
        case ErrorCode::unsupported: return "unsupported";
        case ErrorCode::insufficient_evidence: return "insufficient_evidence";
        case ErrorCode::stale_evidence: return "stale_evidence";
        case ErrorCode::rate_limited: return "rate_limited";
        case ErrorCode::busy: return "busy";
        case ErrorCode::internal_error: return "internal_error";
    }
    return "internal_error";
}

HeadParse parse_head(std::string_view buf, const HeadLimits& lim) {
    HeadParse out;
    const char* method = nullptr;
    const char* target = nullptr;
    std::size_t method_len = 0, target_len = 0;
    int minor = -1;
    std::vector<phr_header> hdrs(lim.max_headers + 1);
    std::size_t nh = hdrs.size();
    const int rc = phr_parse_request(buf.data(), buf.size(), &method, &method_len, &target, &target_len, &minor,
                                     hdrs.data(), &nh, 0);
    auto fail = [&](ApiError e) {
        out.state = HeadParse::State::error;
        out.error = std::move(e);
        return out;
    };
    if (rc == -2) {
        if (buf.size() >= lim.max_head_bytes)
            return fail(err(431, ErrorCode::invalid_request, "headers_too_large", "The request headers are too large."));
        return out;  // incomplete
    }
    if (rc < 0) {
        // -1: malformed, or more headers than the array holds.
        if (nh >= hdrs.size())
            return fail(err(431, ErrorCode::invalid_request, "too_many_headers", "The request has too many headers."));
        return fail(err(400, ErrorCode::invalid_request, "malformed_http", "The request is not valid HTTP/1.x."));
    }
    if (static_cast<std::size_t>(rc) > lim.max_head_bytes)
        return fail(err(431, ErrorCode::invalid_request, "headers_too_large", "The request headers are too large."));
    if (nh > lim.max_headers)
        return fail(err(431, ErrorCode::invalid_request, "too_many_headers", "The request has too many headers."));
    if (minor != 0 && minor != 1)
        return fail(err(505, ErrorCode::invalid_request, "http_version", "Only HTTP/1.0 and HTTP/1.1 are supported."));

    HttpRequest& r = out.request;
    r.method.assign(method, method_len);
    r.minor_version = minor;
    r.head_bytes = static_cast<std::size_t>(rc);
    const std::string_view tgt(target, target_len);
    if (tgt.size() > lim.max_target_bytes)
        return fail(err(414, ErrorCode::invalid_request, "target_too_long", "The request target is too long."));
    const auto q = tgt.find('?');
    r.path = std::string(tgt.substr(0, q));
    if (q != std::string_view::npos) r.query = std::string(tgt.substr(q + 1));
    if (r.path.empty() || r.path[0] != '/' || !std::all_of(r.path.begin(), r.path.end(), path_char) ||
        r.path.find("..") != std::string::npos || r.path.find("//") != std::string::npos)
        return fail(err(400, ErrorCode::invalid_request, "bad_path",
                        "The request path must be a plain /v1/... path (no encoding, '..' or '//')."));
    for (char c : r.query)
        if (!(param_char(c) || c == '=' || c == '&'))
            return fail(err(400, ErrorCode::invalid_request, "bad_query",
                            "Query parameters must be plain name=value pairs without encoding."));

    bool seen_host = false, seen_type = false, seen_expect = false, seen_id = false;
    for (std::size_t i = 0; i < nh; ++i) {
        const auto& h = hdrs[i];
        if (h.name == nullptr)  // obsolete line folding
            return fail(err(400, ErrorCode::invalid_request, "header_folding", "Folded header lines are not accepted."));
        const std::string_view name(h.name, h.name_len);
        const std::string_view raw(h.value, h.value_len);
        for (char c : raw) {
            const auto u = static_cast<unsigned char>(c);
            if ((u < 0x20 && c != '\t') || u >= 0x7f)
                return fail(err(400, ErrorCode::invalid_request, "header_bytes", "Header values must be printable ASCII."));
        }
        const std::string value = trim(raw);
        if (iequals(name, "content-length")) {
            if (r.content_length)
                return fail(err(400, ErrorCode::invalid_request, "duplicate_content_length",
                                "Content-Length must appear once."));
            if (value.empty() || value.size() > 9 ||
                !std::all_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                return fail(err(400, ErrorCode::invalid_request, "bad_content_length", "Content-Length must be a number."));
            r.content_length = static_cast<std::size_t>(std::stoul(value));
        } else if (iequals(name, "transfer-encoding")) {
            return fail(err(501, ErrorCode::unsupported, "transfer_encoding",
                            "Transfer-Encoding is not supported; send a Content-Length body."));
        } else if (iequals(name, "content-type")) {
            if (seen_type)
                return fail(err(400, ErrorCode::invalid_request, "duplicate_content_type", "Content-Type must appear once."));
            seen_type = true;
            r.content_type = lower(trim(std::string_view(value).substr(0, value.find(';'))));
        } else if (iequals(name, "host")) {
            if (seen_host) return fail(err(400, ErrorCode::invalid_request, "duplicate_host", "Host must appear once."));
            seen_host = true;
        } else if (iequals(name, "expect")) {
            if (seen_expect) return fail(err(400, ErrorCode::invalid_request, "duplicate_expect", "Expect must appear once."));
            seen_expect = true;
            if (!iequals(value, "100-continue"))
                return fail(err(417, ErrorCode::invalid_request, "expectation", "Only 'Expect: 100-continue' is supported."));
            r.expect_continue = true;
        } else if (iequals(name, "x-request-id")) {
            if (seen_id)
                return fail(err(400, ErrorCode::invalid_request, "duplicate_request_id", "X-Request-Id must appear once."));
            seen_id = true;
            if (value.empty() || value.size() > 64 || !std::all_of(value.begin(), value.end(), id_char))
                return fail(err(400, ErrorCode::invalid_request, "bad_request_id",
                                "X-Request-Id must be 1-64 characters of [A-Za-z0-9._:-]."));
            r.request_id = value;
        }
    }
    if (minor == 1 && !seen_host)
        return fail(err(400, ErrorCode::invalid_request, "missing_host", "HTTP/1.1 requests need a Host header."));
    if (r.content_length && *r.content_length > lim.max_body_bytes)
        return fail(err(413, ErrorCode::invalid_request, "body_too_large", "The request body is too large."));
    out.state = HeadParse::State::complete;
    return out;
}

std::optional<std::map<std::string, std::string>> parse_query(std::string_view q, std::size_t max_params) {
    std::map<std::string, std::string> out;
    if (q.empty()) return out;
    std::size_t i = 0;
    while (i <= q.size()) {
        const std::size_t j = std::min(q.find('&', i), q.size());
        const std::string_view pair = q.substr(i, j - i);
        const auto eq = pair.find('=');
        if (pair.empty() || eq == 0 || eq == std::string_view::npos) return std::nullopt;
        const std::string name(pair.substr(0, eq));
        const std::string value(pair.substr(eq + 1));
        if (value.empty() || value.find('=') != std::string::npos) return std::nullopt;
        if (!std::all_of(name.begin(), name.end(), param_char) || !std::all_of(value.begin(), value.end(), param_char))
            return std::nullopt;
        if (!out.emplace(name, value).second) return std::nullopt;  // duplicate
        if (out.size() > max_params) return std::nullopt;
        i = j + 1;
    }
    return out;
}

std::string_view status_text(int status) noexcept {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 417: return "Expectation Failed";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string serialize(const HttpResponse& r) {
    std::string o = "HTTP/1.1 " + std::to_string(r.status) + " " + std::string(status_text(r.status)) + "\r\n";
    o += "Content-Type: application/json\r\n";
    o += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    o += "Cache-Control: no-store\r\n";
    o += "X-Content-Type-Options: nosniff\r\n";
    o += "Connection: close\r\n";
    for (const auto& [k, v] : r.headers) o += k + ": " + v + "\r\n";
    o += "\r\n";
    o += r.body;
    return o;
}

}  // namespace osie::api
