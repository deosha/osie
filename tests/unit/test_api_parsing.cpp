// SPDX-License-Identifier: Apache-2.0
// Local API request parsing: HTTP framing policy, query strings and the
// strict JSON body parser. Pure functions; no sockets.
#include <gtest/gtest.h>

#include <string>

#include "osie/api/http.hpp"
#include "osie/api/json_in.hpp"

using namespace osie;
using namespace osie::api;

namespace {

HeadParse head(const std::string& s, HeadLimits lim = {}) { return parse_head(s, lim); }

void expect_error(const std::string& req, int status, const std::string& reason) {
    const auto h = head(req);
    ASSERT_EQ(h.state, HeadParse::State::error) << req;
    EXPECT_EQ(h.error.status, status) << req;
    EXPECT_EQ(h.error.reason, reason) << req;
}

}  // namespace

// --------------------------------------------------------------------- HTTP ---

TEST(ApiHttp, ParsesAValidGetAndPost) {
    auto g = head("GET /v1/health/live HTTP/1.1\r\nHost: localhost\r\nUser-Agent: curl/8\r\n\r\n");
    ASSERT_EQ(g.state, HeadParse::State::complete);
    EXPECT_EQ(g.request.method, "GET");
    EXPECT_EQ(g.request.path, "/v1/health/live");
    EXPECT_TRUE(g.request.query.empty());
    EXPECT_FALSE(g.request.content_length);
    auto p = head("POST /v1/diagnose HTTP/1.1\r\nHost: localhost\r\nContent-Type: Application/JSON; charset=utf-8\r\n"
                  "Content-Length: 12\r\nX-Request-Id: abc-1\r\nExpect: 100-continue\r\n\r\n{\"question\"");
    ASSERT_EQ(p.state, HeadParse::State::complete);
    EXPECT_EQ(*p.request.content_type, "application/json");
    EXPECT_EQ(*p.request.content_length, 12U);
    EXPECT_EQ(p.request.request_id, "abc-1");
    EXPECT_TRUE(p.request.expect_continue);
    EXPECT_LT(p.request.head_bytes, 200U);
    auto q = head("GET /v1/system/history?window_seconds=60&limit=5 HTTP/1.0\r\n\r\n");
    ASSERT_EQ(q.state, HeadParse::State::complete) << "HTTP/1.0 needs no Host";
    EXPECT_EQ(q.request.query, "window_seconds=60&limit=5");
}

TEST(ApiHttp, IncompleteHeadsWaitAndOversizedHeadsAreRefused) {
    EXPECT_EQ(head("GET /v1/health/live HTTP/1.1\r\nHost: x\r\n").state, HeadParse::State::incomplete);
    HeadLimits lim;
    lim.max_head_bytes = 256;
    const std::string big = "GET /v1/health/live HTTP/1.1\r\nHost: x\r\nX-Pad: " + std::string(400, 'a');
    const auto h = parse_head(big, lim);
    ASSERT_EQ(h.state, HeadParse::State::error);
    EXPECT_EQ(h.error.status, 431);
    std::string many = "GET /v1/health/live HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 40; ++i) many += "X-H" + std::to_string(i) + ": v\r\n";
    many += "\r\n";
    EXPECT_EQ(head(many).error.status, 431);
}

TEST(ApiHttp, FramingAndHeaderPolicy) {
    expect_error("GET /v1/health/live HTTP/1.1\r\n\r\n", 400, "missing_host");
    expect_error("GET /v1/health/live HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, "duplicate_host");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400,
                 "duplicate_content_length");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", 400,
                 "duplicate_content_length");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", 400, "bad_content_length");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nContent-Length: 1e3\r\n\r\n", 400, "bad_content_length");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501, "transfer_encoding");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nContent-Type: a/b\r\nContent-Type: c/d\r\n\r\n", 400,
                 "duplicate_content_type");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nExpect: something\r\n\r\n", 417, "expectation");
    expect_error("POST /v1/diagnose HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999\r\n\r\n", 413, "body_too_large");
    expect_error("GET /v1/health/live HTTP/1.2\r\nHost: a\r\n\r\n", 505, "http_version");
    expect_error("GET /v1/health/live HTTP/2.0\r\nHost: a\r\n\r\n", 400, "malformed_http");
    expect_error("GET /v1/health/live HTTP/1.1\r\nHost: a\r\nX-Request-Id: has space\r\n\r\n", 400, "bad_request_id");
    expect_error("GET /v1/health/live HTTP/1.1\r\nHost: a\r\nX-Folded: a\r\n continued\r\n\r\n", 400, "header_folding");
    expect_error("GET /v1/health/live HTTP/1.1\r\nHost: a\r\nX-Bin: \x80\x81\r\n\r\n", 400, "header_bytes");
    expect_error("garbage\r\n\r\n", 400, "malformed_http");
}

TEST(ApiHttp, PathsAreLiteralAndQueriesPlain) {
    for (const char* t : {"/v1/../etc/passwd", "/v1//health", "/v1/health%2flive", "/v1/%2e%2e/x", "relative",
                          "/v1/health/live#frag", "/v1/a b"}) {
        const auto h = head(std::string("GET ") + t + " HTTP/1.1\r\nHost: a\r\n\r\n");
        EXPECT_EQ(h.state, HeadParse::State::error) << t;
    }
    expect_error("GET /v1/system/history?metric=%2e HTTP/1.1\r\nHost: a\r\n\r\n", 400, "bad_query");
    expect_error("GET /" + std::string(600, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n", 414, "target_too_long");
    // Embedded NUL in the request line or a header.
    const std::string nul_line = std::string("GET /v1/health") + '\0' + "/live HTTP/1.1\r\nHost: a\r\n\r\n";
    EXPECT_EQ(head(nul_line).state, HeadParse::State::error);
    const std::string nul_hdr = std::string("GET /v1/health/live HTTP/1.1\r\nHost: a") + '\0' + "b\r\n\r\n";
    EXPECT_EQ(head(nul_hdr).state, HeadParse::State::error);

    auto q = parse_query("window_seconds=60&limit=5&metric=cpu_busy_pct");
    ASSERT_TRUE(q);
    EXPECT_EQ(q->at("limit"), "5");
    for (const char* bad : {"a=1&a=2", "a", "=1", "a=", "a=1&&b=2", "a=b=c", "a=%41", "a=1&b=2&c=3&d=4&e=5&f=6&g=7&h=8&i=9"})
        EXPECT_FALSE(parse_query(bad)) << bad;
    EXPECT_TRUE(parse_query("")->empty());
}

TEST(ApiHttp, ResponsesAlwaysCloseAndAreNotCached) {
    HttpResponse r;
    r.status = 429;
    r.body = "{}";
    r.headers.emplace_back("Retry-After", "1");
    const auto s = serialize(r);
    EXPECT_EQ(s.rfind("HTTP/1.1 429 Too Many Requests\r\n", 0), 0U);
    for (const char* h : {"Content-Type: application/json\r\n", "Content-Length: 2\r\n", "Connection: close\r\n",
                          "Cache-Control: no-store\r\n", "Retry-After: 1\r\n"})
        EXPECT_NE(s.find(h), std::string::npos) << h;
    EXPECT_EQ(s.substr(s.size() - 2), "{}");
}

// --------------------------------------------------------------------- JSON ---

TEST(ApiJson, AcceptsFlatObjects) {
    auto o = parse_flat_object(R"({"question":"why is it slow?","max_evidence_age_ms":10000,"include_evidence":false,
                                   "session_id":null, "request_id" : "r-1"})");
    ASSERT_TRUE(o.ok()) << o.error().describe();
    EXPECT_EQ(std::get<std::string>(o->at("question")), "why is it slow?");
    EXPECT_EQ(std::get<std::int64_t>(o->at("max_evidence_age_ms")), 10000);
    EXPECT_FALSE(std::get<bool>(o->at("include_evidence")));
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(o->at("session_id")));
    auto u = parse_flat_object(R"({"q":"caf\u00e9 \ud83d\ude00 ü"})");
    ASSERT_TRUE(u.ok()) << u.error().describe();
    EXPECT_EQ(std::get<std::string>(u->at("q")), "caf\xc3\xa9 \xf0\x9f\x98\x80 \xc3\xbc");
    EXPECT_TRUE(parse_flat_object("{}").ok());
}

TEST(ApiJson, RejectsEverythingElse) {
    for (const char* bad : {
             "", "[]", "\"x\"", "{", "{\"a\":1,}", "{\"a\":1 \"b\":2}", "{\"a\":{\"b\":1}}", "{\"a\":[1]}",
             "{\"a\":1.5}", "{\"a\":1e3}", "{\"a\":01}", "{\"a\":99999999999999999999}", "{\"a\":1,\"a\":2}",
             "{\"a\":\"\\u0000\"}",     // embedded NUL
             "{\"a\":\"line\\nbreak\"}",  // escaped control character
             "{\"a\":\"\\ud800\"}",     // unpaired surrogate
             "{\"a\":\"\\udc00\"}", "{\"a\":\"\\x41\"}", "{\"a\":tru}", "{\"a\":1} trailing", "{a:1}",
             "{\"a\":\"\xc0\xaf\"}",    // overlong encoding of '/'
             "{\"a\":\"\xed\xa0\x80\"}",  // UTF-8 encoded surrogate
             "{\"a\":\"\xff\"}", "{\"a\":\"\xe2\x82\"}", "{\"a\":\"\x7f\"}"}) {
        EXPECT_FALSE(parse_flat_object(bad).ok()) << bad;
    }
    const std::string raw_ctrl = std::string("{\"a\":\"x") + '\x01' + "\"}";
    EXPECT_FALSE(parse_flat_object(raw_ctrl).ok());
    const std::string raw_nul = std::string("{\"a\":\"x") + '\0' + "\"}";
    EXPECT_FALSE(parse_flat_object(raw_nul).ok());
    JsonLimits lim;
    lim.max_members = 2;
    EXPECT_FALSE(parse_flat_object(R"({"a":1,"b":2,"c":3})", lim).ok());
    lim = {};
    lim.max_string_bytes = 10;
    EXPECT_FALSE(parse_flat_object(R"({"a":"01234567890123"})", lim).ok());
    lim = {};
    lim.max_bytes = 8;
    EXPECT_FALSE(parse_flat_object(R"({"a":"0123"})", lim).ok());
    // Errors describe the problem without echoing the input.
    const auto e = parse_flat_object(R"({"secret-looking-key":{"x":1}})");
    ASSERT_FALSE(e.ok());
    EXPECT_EQ(e.error().message.find("secret"), std::string::npos);
}
