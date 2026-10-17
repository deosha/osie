// SPDX-License-Identifier: Apache-2.0
// What `osie remote` reads from another host: the HTTP response parser
// (framing, bounds, malformed and hostile replies), the strict JSON reader,
// the re-indenter (bytes of strings and numbers kept), and the text
// renderers, which read only the fields the contract allows.
#include <gtest/gtest.h>

#include <string>

#include "osie/remote/cli.hpp"
#include "osie/remote/http.hpp"
#include "osie/util/json.hpp"

using namespace osie::remote;

namespace {

std::string reply(int status, const std::string& body, const std::string& extra = "",
                  const std::string& type = "application/json") {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + type + "\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

const std::string kBody = R"({"schema_version":"1.0","status":"completed"})";

}  // namespace

TEST(RemoteHttp, ParsesAgentReplies) {
    auto r = parse_reply(reply(200, kBody), false);
    ASSERT_EQ(r.state, ReplyParse::State::complete) << r.error;
    EXPECT_EQ(r.reply.status, 200);
    EXPECT_EQ(r.reply.body, kBody);
    // A real server response (as osie::api::serialize writes it).
    const std::string real = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: " +
                             std::to_string(kBody.size()) +
                             "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n" + kBody;
    r = parse_reply(real, true);
    ASSERT_EQ(r.state, ReplyParse::State::complete) << r.error;
    EXPECT_EQ(r.reply.status, 403);
    // HTTP/1.0, header case, parameters on the media type.
    r = parse_reply("HTTP/1.0 503 S\r\ncontent-type: Application/JSON; charset=utf-8\r\ncontent-length: " +
                        std::to_string(kBody.size()) + "\r\nRetry-After: 1\r\n\r\n" + kBody,
                    true);
    ASSERT_EQ(r.state, ReplyParse::State::complete) << r.error;
    EXPECT_EQ(r.reply.status, 503);
}

TEST(RemoteHttp, BodyBytesArePreservedExactly) {
    // Unknown fields, odd whitespace, escapes and exponent numbers survive.
    const std::string body = "{ \"schema_version\" : \"1.0\",\n\t\"future_field\":{\"x\":[1.50e+3,-0,\"\\u00e9\\n\"]},"
                             "\"text\":\"caf\xc3\xa9 \\\"q\\\"\"  }";
    auto r = parse_reply(reply(200, body), true);
    ASSERT_EQ(r.state, ReplyParse::State::complete) << r.error;
    EXPECT_EQ(r.reply.body, body);
}

TEST(RemoteHttp, IncrementalAndTruncated) {
    const std::string full = reply(200, kBody);
    for (std::size_t cut = 0; cut < full.size(); ++cut) {
        const auto r = parse_reply(full.substr(0, cut), false);
        EXPECT_EQ(r.state, ReplyParse::State::incomplete) << cut;
        const auto e = parse_reply(full.substr(0, cut), true);
        EXPECT_EQ(e.state, ReplyParse::State::error) << "truncated at " << cut << " must be an error at EOF";
    }
    EXPECT_EQ(parse_reply("", true).error, "the connection closed without a response");
}

TEST(RemoteHttp, MalformedRepliesAreProtocolErrors) {
    const std::vector<std::string> bad = {
        "garbage\r\n\r\n",
        "SSH-2.0-OpenSSH_9.6\r\n",
        "HTTP/2 200 OK\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + kBody,  // no Content-Length
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kBody.size()) + "\r\n\r\n" + kBody,  // no Content-Type
        reply(200, kBody, "", "text/html"),
        reply(200, kBody, "Content-Length: 46\r\n"),               // duplicate Content-Length
        reply(200, kBody, "Content-Type: application/json\r\n"),  // duplicate Content-Type
        reply(200, kBody, "Transfer-Encoding: chunked\r\n"),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1e3\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length:  +46\r\n\r\n" + kBody,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 99999999999\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n" + reply(200, kBody),
        "HTTP/1.1 101 Switching\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 600 X\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
        reply(200, kBody) + "extra",                           // data after the body
        reply(200, kBody) + reply(200, kBody),                 // pipelined second response
        reply(200, "{\"schema_version\":\"1.0\""),             // invalid JSON
        reply(200, "[1,2]"),                                   // not an object
        reply(200, "{\"status\":\"ok\"}"),                     // no schema_version
        reply(200, "{\"schema_version\":1}"),                  // schema_version not a string
        reply(200, "{\"schema_version\":\"1.0\",\"a\":1,\"a\":2}"),  // duplicate key
        reply(200, "{\"schema_version\":\"1.0\"} {}"),
        reply(200, "{\"schema_version\":\"1.0\",\"x\":\"\x01\"}"),
        reply(200, "{\"schema_version\":\"1.0\",\"x\":\"\xff\"}"),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX: a\x01b\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n folded\r\nContent-Length: 2\r\n\r\n{}",
    };
    for (const auto& b : bad) {
        const auto r = parse_reply(b, true);
        EXPECT_EQ(r.state, ReplyParse::State::error) << b;
        EXPECT_EQ(r.error.find("secret"), std::string::npos);
    }
}

TEST(RemoteHttp, OversizedRepliesAreRefusedEarly) {
    // A declared length over the bound is refused before the body arrives.
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                             std::to_string(kMaxResponseBytes) + "\r\n\r\n";
    auto r = parse_reply(head, false);
    EXPECT_EQ(r.state, ReplyParse::State::error) << "head + body exceeds 4 MiB";
    // Bytes beyond the bound are refused whatever the headers say.
    r = parse_reply(std::string(kMaxResponseBytes + 1, 'x'), false);
    EXPECT_EQ(r.state, ReplyParse::State::error);
    // A head that never ends.
    std::string endless = "HTTP/1.1 200 OK\r\n";
    while (endless.size() < kMaxResponseHeadBytes + 10) endless += "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
    r = parse_reply(endless, false);
    EXPECT_EQ(r.state, ReplyParse::State::error);
    // Too many headers.
    std::string many = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n";
    for (int i = 0; i < 40; ++i) many += "X-" + std::to_string(i) + ": v\r\n";
    r = parse_reply(many + "\r\n{}", true);
    EXPECT_EQ(r.state, ReplyParse::State::error);
    // A body exactly at the bound is accepted.
    std::string body = "{\"schema_version\":\"1.0\",\"pad\":\"";
    const std::string h2_probe = reply(200, "");
    const std::size_t room = kMaxResponseBytes - h2_probe.size() - 16;
    body += std::string(room - body.size() - 2, 'a') + "\"}";
    const std::string at = reply(200, body);
    ASSERT_LE(at.size(), kMaxResponseBytes);
    r = parse_reply(at, true);
    EXPECT_EQ(r.state, ReplyParse::State::complete) << r.error;
}

TEST(RemoteHttp, JsonReaderIsStrict) {
    for (const char* ok : {"{}", "[]", "0", "-0", "1.5e-3", "\"\\ud83d\\ude00\"", "{\"a\":[true,false,null]}",
                           " { \"a\" : 1 } ", "\"\xe2\x82\xac\""})
        EXPECT_TRUE(parse_json(ok)) << ok;
    for (const char* bad : {"", "{", "}", "01", "1.", ".5", "+1", "1e", "NaN", "Infinity", "[1,]", "{\"a\":1,}", "{'a':1}",
                            "\"\\x41\"", "\"\\ud800\"", "\"\\udc00\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf5\x80\x80\x80\"",
                            "\"a\tb\"", "{\"a\" 1}", "[1 2]", "tru", "nul", "{} x", "\"\\u12\""})
        EXPECT_FALSE(parse_json(bad)) << bad;
    std::string deep(kMaxJsonDepth + 2, '[');
    deep += std::string(kMaxJsonDepth + 2, ']');
    EXPECT_FALSE(parse_json(deep)) << "depth limit";
    std::string ok_deep(kMaxJsonDepth, '[');
    ok_deep += std::string(kMaxJsonDepth, ']');
    EXPECT_TRUE(parse_json(ok_deep));
    auto v = parse_json(R"({"s":"a\"b\\c\/\n\u00e9","n":-1.25E+2})");
    ASSERT_TRUE(v);
    EXPECT_EQ(*v->str("s"), "a\"b\\c/\n\xc3\xa9");
    EXPECT_EQ(v->get("n")->text, "-1.25E+2") << "numbers keep their source text";
    EXPECT_EQ(v->get("missing"), nullptr);
    EXPECT_EQ(v->str("n"), nullptr) << "not a string";
}

TEST(RemoteHttp, PrettyPrintKeepsTokensByteForByte) {
    const std::string src = R"({"a":[1.50e+3,-0,{}],"b":"x, y: {z} \"q\" \\","c":{"d":[]},"e":"caf\u00e9"})";
    const std::string p = pretty_json(src);
    ASSERT_TRUE(parse_json(p)) << p;
    // Removing whitespace outside strings gives back the source.
    std::string squeezed;
    bool in = false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (in) {
            squeezed += c;
            if (c == '\\') squeezed += p[++i];
            else if (c == '"') in = false;
        } else if (c == '"') {
            in = true;
            squeezed += c;
        } else if (c != ' ' && c != '\n') {
            squeezed += c;
        }
    }
    EXPECT_EQ(squeezed, src);
    EXPECT_NE(p.find("\n  \"a\": [\n"), std::string::npos) << p;
    EXPECT_NE(p.find("1.50e+3"), std::string::npos);
    EXPECT_NE(p.find("\"x, y: {z} \\\"q\\\" \\\\\""), std::string::npos) << "string contents untouched";
}

TEST(RemoteHttp, RequestsAreWellFormed) {
    const auto g = http_get("/v1/health/ready");
    EXPECT_EQ(g.rfind("GET /v1/health/ready HTTP/1.1\r\nHost: localhost\r\n", 0), 0u);
    EXPECT_EQ(g.substr(g.size() - 4), "\r\n\r\n");
    EXPECT_EQ(g.find("Expect"), std::string::npos) << "never sends Expect: no 1xx replies";
    DiagnoseRequest d;
    d.question = "why \"slow\"?\n$(rm -rf /) `id` \xc3\xa9";
    d.session_id = "s-1";
    d.max_evidence_age_ms = 5000;
    const auto body = diagnose_body(d);
    auto v = parse_json(body);
    ASSERT_TRUE(v) << body;
    EXPECT_EQ(*v->str("question"), d.question) << "the question travels only as a JSON string";
    EXPECT_EQ(*v->str("session_id"), "s-1");
    EXPECT_EQ(v->get("max_evidence_age_ms")->text, "5000");
    const auto post = http_post_json("/v1/diagnose", body);
    EXPECT_NE(post.find("Content-Length: " + std::to_string(body.size()) + "\r\n"), std::string::npos);
    EXPECT_EQ(post.substr(post.size() - body.size()), body);
    DiagnoseRequest minimal;
    minimal.question = "q";
    EXPECT_EQ(diagnose_body(minimal), "{\"question\":\"q\"}");
}

TEST(RemoteHttp, HistoryTargetPassesOnlyWellFormedParameters) {
    EXPECT_EQ(*history_target({}, {}, {}), "/v1/system/history");
    EXPECT_EQ(*history_target(std::string("600"), std::string("10"), std::string("cpu_busy_pct")),
              "/v1/system/history?window_seconds=600&limit=10&metric=cpu_busy_pct");
    EXPECT_EQ(*history_target({}, std::string("99999"), {}), "/v1/system/history?limit=99999") << "ranges are the agent's";
    for (const char* bad : {"", "-1", "1&metric=x", "1 2", "%31", "1;", "a"})
        EXPECT_FALSE(history_target(std::string(bad), {}, {})) << bad;
    for (const std::string bad : {std::string(), std::string("CPU"), std::string("cpu busy"), std::string("cpu&x=1"), std::string("../x"), std::string(33, 'a')})
        EXPECT_FALSE(history_target({}, {}, std::string(bad))) << bad;
}

TEST(RemoteHttp, AnswerTextReadsOnlyAllowedFields) {
    const std::string body = R"({"schema_version":"1.0","status":"partial","answer":{"text":"CPU is busy.\n","summary":"S"},
        "limitations":[{"code":"missing_evidence","detail":"no PSI"},{"code":"redacted"}],
        "evidence":[{"value":"SHOULD-NOT-APPEAR"}],"future":{"text":"NOPE"}})";
    const auto t = render_answer_text(body);
    EXPECT_NE(t.find("CPU is busy.\n"), std::string::npos);
    EXPECT_NE(t.find("(status: partial)"), std::string::npos);
    EXPECT_NE(t.find("  - missing_evidence: no PSI\n"), std::string::npos);
    EXPECT_NE(t.find("  - redacted\n"), std::string::npos);
    EXPECT_EQ(t.find("SHOULD-NOT-APPEAR"), std::string::npos);
    EXPECT_EQ(t.find("NOPE"), std::string::npos);
    EXPECT_EQ(t.find("summary"), std::string::npos);
    // Completed answers do not print a status line; missing fields are tolerated.
    EXPECT_EQ(render_answer_text(R"({"schema_version":"1.0","status":"completed","answer":{"text":"ok"}})"), "ok\n");
    EXPECT_EQ(render_answer_text(R"({"schema_version":"1.0","answer":{"text":7},"limitations":"x"})"), "(no answer text)\n");
}

TEST(RemoteHttp, ErrorTextReadsOnlyTheErrorObject) {
    const auto e = render_error_text(
        R"({"schema_version":"1.0","error":{"code":"forbidden","reason":"diagnostics_not_permitted","message":"Not allowed.","retryable":false,"new":1}})",
        403);
    EXPECT_EQ(e, "agent error 403: forbidden (diagnostics_not_permitted): Not allowed.\n");
    EXPECT_EQ(render_error_text(R"({"schema_version":"1.0","error":{"code":"busy","retryable":true}})", 503),
              "agent error 503: busy [retryable]\n");
    EXPECT_EQ(render_error_text(R"({"schema_version":"1.0","status":"starting"})", 503),
              "agent answered HTTP 503 (status: starting)\n");
}
