// SPDX-License-Identifier: Apache-2.0
#include "osie/remote/http.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "osie/util/json.hpp"
#include "picohttpparser.h"

namespace osie::remote {
namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

ReplyParse fail(std::string why) {
    ReplyParse r;
    r.state = ReplyParse::State::error;
    r.error = std::move(why);
    return r;
}

// ------------------------------------------------------------ JSON parser ---

class Parser {
public:
    Parser(std::string_view t, std::size_t max_depth) : t_(t), max_depth_(max_depth) {}

    std::optional<JsonNode> document() {
        JsonNode v;
        if (!value(v, 0)) return std::nullopt;
        ws();
        if (i_ != t_.size()) return std::nullopt;
        return v;
    }

private:
    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool lit(std::string_view s) {
        if (t_.substr(i_, s.size()) != s) return false;
        i_ += s.size();
        return true;
    }
    bool value(JsonNode& v, std::size_t depth) {
        if (depth > max_depth_) return false;
        ws();
        if (i_ >= t_.size()) return false;
        const char c = t_[i_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') {
            v.kind = JsonNode::Kind::string;
            return string(v.text);
        }
        if (c == 't') { v.kind = JsonNode::Kind::boolean; v.b = true; return lit("true"); }
        if (c == 'f') { v.kind = JsonNode::Kind::boolean; v.b = false; return lit("false"); }
        if (c == 'n') { v.kind = JsonNode::Kind::null; return lit("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return false;
    }
    bool object(JsonNode& v, std::size_t depth) {
        v.kind = JsonNode::Kind::object;
        ++i_;
        ws();
        if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
        std::set<std::string> seen;
        while (true) {
            ws();
            if (i_ >= t_.size() || t_[i_] != '"') return false;
            std::string k;
            if (!string(k)) return false;
            if (!seen.insert(k).second) return false;
            ws();
            if (i_ >= t_.size() || t_[i_] != ':') return false;
            ++i_;
            JsonNode child;
            if (!value(child, depth + 1)) return false;
            v.members.emplace_back(std::move(k), std::move(child));
            ws();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == '}') { ++i_; return true; }
            return false;
        }
    }
    bool array(JsonNode& v, std::size_t depth) {
        v.kind = JsonNode::Kind::array;
        ++i_;
        ws();
        if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
        while (true) {
            JsonNode child;
            if (!value(child, depth + 1)) return false;
            v.items.push_back(std::move(child));
            ws();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == ']') { ++i_; return true; }
            return false;
        }
    }
    bool digits() {
        const std::size_t s = i_;
        while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        return i_ > s;
    }
    bool number(JsonNode& v) {
        const std::size_t s = i_;
        if (t_[i_] == '-') ++i_;
        if (i_ >= t_.size()) return false;
        if (t_[i_] == '0') ++i_;
        else if (!digits()) return false;
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            if (!digits()) return false;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            if (!digits()) return false;
        }
        v.kind = JsonNode::Kind::number;
        v.text = std::string(t_.substr(s, i_ - s));
        return true;
    }
    static void put_utf8(std::string& o, std::uint32_t cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& out) {
        if (i_ + 4 > t_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    /// One UTF-8 sequence starting at i_ (validated: shortest form, no
    /// surrogates, at most U+10FFFF).
    bool utf8(std::string& o) {
        const auto b0 = static_cast<unsigned char>(t_[i_]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; cp = b0 & 0x1Fu; }
        else if (b0 >= 0xE0 && b0 <= 0xEF) { len = 3; cp = b0 & 0x0Fu; }
        else if (b0 >= 0xF0 && b0 <= 0xF4) { len = 4; cp = b0 & 0x07u; }
        else return false;
        if (i_ + len > t_.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(t_[i_ + k]);
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        o.append(t_.substr(i_, len));
        i_ += len;
        return true;
    }
    bool string(std::string& o) {
        ++i_;  // opening quote
        while (i_ < t_.size()) {
            const auto c = static_cast<unsigned char>(t_[i_]);
            if (c == '"') { ++i_; return true; }
            if (c < 0x20) return false;
            if (c >= 0x80) {
                if (!utf8(o)) return false;
                continue;
            }
            if (c != '\\') { o += static_cast<char>(c); ++i_; continue; }
            if (++i_ >= t_.size()) return false;
            const char e = t_[i_++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t lo = 0;
                        if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    std::string_view t_;
    std::size_t i_{0};
    std::size_t max_depth_;
};

bool target_value_ok(std::string_view v, bool digits_only) {
    if (v.empty() || v.size() > 32) return false;
    return std::all_of(v.begin(), v.end(), [&](char c) {
        return (c >= '0' && c <= '9') || (!digits_only && ((c >= 'a' && c <= 'z') || c == '_'));
    });
}

}  // namespace

const JsonNode* JsonNode::get(std::string_view k) const {
    if (kind != Kind::object) return nullptr;
    for (const auto& [name, v] : members)
        if (name == k) return &v;
    return nullptr;
}

const std::string* JsonNode::str(std::string_view k) const {
    const auto* v = get(k);
    return v != nullptr && v->kind == Kind::string ? &v->text : nullptr;
}

std::optional<JsonNode> parse_json(std::string_view text, std::size_t max_depth) {
    return Parser(text, max_depth).document();
}

std::string pretty_json(std::string_view t) {
    std::string o;
    o.reserve(t.size() + t.size() / 4);
    int depth = 0;
    auto nl = [&] {
        o += '\n';
        o.append(static_cast<std::size_t>(std::max(depth, 0)) * 2, ' ');
    };
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '"') {  // copy the string verbatim, escapes included
            const std::size_t s = i++;
            while (i < t.size() && t[i] != '"') i += t[i] == '\\' ? 2 : 1;
            o.append(t.substr(s, std::min(i, t.size() - 1) - s + 1));
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '{' || c == '[') {
            // Empty containers stay on one line.
            std::size_t j = i + 1;
            while (j < t.size() && (t[j] == ' ' || t[j] == '\t' || t[j] == '\n' || t[j] == '\r')) ++j;
            if (j < t.size() && (t[j] == '}' || t[j] == ']')) {
                o += c;
                o += t[j];
                i = j;
                continue;
            }
            o += c;
            ++depth;
            nl();
        } else if (c == '}' || c == ']') {
            --depth;
            nl();
            o += c;
        } else if (c == ',') {
            o += c;
            nl();
        } else if (c == ':') {
            o += ": ";
        } else {
            o += c;
        }
    }
    return o;
}

ReplyParse parse_reply(std::string_view buf, bool eof, std::size_t max_bytes) {
    if (buf.size() > max_bytes) return fail("the response is larger than " + std::to_string(max_bytes) + " bytes");
    int minor = -1, status = 0;
    const char* msg = nullptr;
    std::size_t msg_len = 0;
    phr_header headers[kMaxResponseHeaders];
    std::size_t num = kMaxResponseHeaders;
    const std::size_t head_view = std::min(buf.size(), kMaxResponseHeadBytes);
    const int rc = phr_parse_response(buf.data(), head_view, &minor, &status, &msg, &msg_len, headers, &num, 0);
    if (rc == -1) return fail("malformed HTTP response head");
    if (rc == -2) {
        if (buf.size() >= kMaxResponseHeadBytes) return fail("the response head is too large or has too many headers");
        if (eof) return fail(buf.empty() ? "the connection closed without a response" : "truncated response head");
        return ReplyParse{};
    }
    if (minor != 0 && minor != 1) return fail("unsupported HTTP version");
    if (status < 200 || status > 599) return fail("unexpected HTTP status " + std::to_string(status));
    std::optional<std::size_t> length;
    bool json_type = false;
    for (std::size_t k = 0; k < num; ++k) {
        const std::string_view name(headers[k].name == nullptr ? "" : headers[k].name, headers[k].name_len);
        const std::string_view value(headers[k].value, headers[k].value_len);
        if (headers[k].name == nullptr) return fail("folded header line");
        for (unsigned char ch : value)
            if (ch < 0x20 || ch >= 0x7f) return fail("non-printable byte in a header");
        if (ieq(name, "Transfer-Encoding")) return fail("Transfer-Encoding is not accepted (Content-Length only)");
        if (ieq(name, "Content-Length")) {
            if (length) return fail("more than one Content-Length");
            if (value.empty() || value.size() > 9) return fail("invalid Content-Length");
            std::size_t n = 0;
            for (char ch : value) {
                if (ch < '0' || ch > '9') return fail("invalid Content-Length");
                n = n * 10 + static_cast<std::size_t>(ch - '0');
            }
            length = n;
        } else if (ieq(name, "Content-Type")) {
            if (json_type) return fail("more than one Content-Type");
            std::string_view mt = value.substr(0, value.find(';'));
            while (!mt.empty() && mt.back() == ' ') mt.remove_suffix(1);
            if (!ieq(mt, "application/json")) return fail("the response is not application/json");
            json_type = true;
        }
    }
    if (!length) return fail("the response has no Content-Length");
    if (!json_type) return fail("the response has no Content-Type");
    const auto head = static_cast<std::size_t>(rc);
    if (head + *length > max_bytes) return fail("the response is larger than " + std::to_string(max_bytes) + " bytes");
    const std::size_t have = buf.size() - head;
    if (have > *length) return fail("data after the response body");
    if (have < *length) {
        if (eof) return fail("truncated response body");
        return ReplyParse{};
    }
    const std::string_view body = buf.substr(head);
    const auto doc = parse_json(body);
    if (!doc) return fail("the response body is not valid JSON");
    if (doc->kind != JsonNode::Kind::object || doc->str("schema_version") == nullptr)
        return fail("the response is not an osie agent response (no schema_version)");
    ReplyParse r;
    r.state = ReplyParse::State::complete;
    r.reply.status = status;
    r.reply.body = std::string(body);
    return r;
}

std::string http_get(std::string_view target) {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nUser-Agent: osie-remote\r\n"
           "Accept: application/json\r\nConnection: close\r\n\r\n";
}

std::string http_post_json(std::string_view target, std::string_view body) {
    return "POST " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nUser-Agent: osie-remote\r\n"
           "Accept: application/json\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

std::string diagnose_body(const DiagnoseRequest& r) {
    json::Writer w;
    w.begin_object().kv("question", r.question);
    if (!r.session_id.empty()) w.kv("session_id", r.session_id);
    if (r.max_evidence_age_ms) w.kv("max_evidence_age_ms", static_cast<std::int64_t>(*r.max_evidence_age_ms));
    w.end_object();
    return w.take();
}

std::optional<std::string> history_target(const std::optional<std::string>& window_seconds,
                                          const std::optional<std::string>& limit,
                                          const std::optional<std::string>& metric) {
    std::string q;
    auto add = [&](const char* k, const std::optional<std::string>& v, bool digits) {
        if (!v) return true;
        if (!target_value_ok(*v, digits)) return false;
        q += (q.empty() ? "?" : "&") + std::string(k) + "=" + *v;
        return true;
    };
    if (!add("window_seconds", window_seconds, true) || !add("limit", limit, true) || !add("metric", metric, false))
        return std::nullopt;
    return "/v1/system/history" + q;
}

}  // namespace osie::remote
