// SPDX-License-Identifier: Apache-2.0
#include "osie/api/json_in.hpp"

#include <limits>
#include <optional>

namespace osie::api {
namespace {

class Parser {
public:
    Parser(std::string_view t, const JsonLimits& l) : t_(t), lim_(l) {}

    Result<JsonObject> object() {
        JsonObject out;
        ws();
        if (!eat('{')) return fail("expected a JSON object");
        ws();
        if (eat('}')) return finish(std::move(out));
        while (true) {
            ws();
            if (peek() != '"') return fail("expected a member name");
            auto key = string();
            if (!key) return key.error();
            ws();
            if (!eat(':')) return fail("expected ':' after a member name");
            ws();
            auto v = scalar();
            if (!v) return v.error();
            if (!out.emplace(std::move(key).value(), std::move(v).value()).second) return fail("duplicate member name");
            if (out.size() > lim_.max_members) return fail("too many members");
            ws();
            if (eat(',')) continue;
            if (eat('}')) return finish(std::move(out));
            return fail("expected ',' or '}'");
        }
    }

private:
    Result<JsonObject> finish(JsonObject o) {
        ws();
        if (i_ != t_.size()) return fail("unexpected content after the object");
        return o;
    }
    static Error fail(const char* m) { return Error{Errc::invalid_argument, std::string("invalid JSON: ") + m, 0}; }
    [[nodiscard]] char peek() const { return i_ < t_.size() ? t_[i_] : '\0'; }
    bool eat(char c) {
        if (peek() == c && i_ < t_.size()) {
            ++i_;
            return true;
        }
        return false;
    }
    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool literal(std::string_view w) {
        if (t_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }

    Result<JsonScalar> scalar() {
        const char c = peek();
        if (c == '"') {
            auto s = string();
            if (!s) return s.error();
            return JsonScalar{std::move(s).value()};
        }
        if (c == '{' || c == '[') return fail("nested objects and arrays are not accepted");
        if (literal("true")) return JsonScalar{true};
        if (literal("false")) return JsonScalar{false};
        if (literal("null")) return JsonScalar{nullptr};
        if (c == '-' || (c >= '0' && c <= '9')) return integer();
        return fail("expected a string, integer, boolean or null");
    }

    Result<JsonScalar> integer() {
        bool neg = eat('-');
        if (!(peek() >= '0' && peek() <= '9')) return fail("expected a digit");
        if (peek() == '0' && i_ + 1 < t_.size() && t_[i_ + 1] >= '0' && t_[i_ + 1] <= '9') return fail("leading zeros");
        std::uint64_t v = 0;
        const std::uint64_t lim = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (neg ? 1 : 0);
        while (peek() >= '0' && peek() <= '9') {
            const auto d = static_cast<std::uint64_t>(t_[i_++] - '0');
            if (v > (lim - d) / 10) return fail("integer out of range");
            v = v * 10 + d;
        }
        if (peek() == '.' || peek() == 'e' || peek() == 'E') return fail("numbers must be integers");
        if (neg) return JsonScalar{v == lim ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(v)};
        return JsonScalar{static_cast<std::int64_t>(v)};
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

    std::optional<std::uint32_t> hex4() {
        if (i_ + 4 > t_.size()) return std::nullopt;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    }

    /// One UTF-8 sequence starting at i_ (validated: no overlongs, no surrogates, <= U+10FFFF).
    bool utf8(std::string& o) {
        const auto b0 = static_cast<unsigned char>(t_[i_]);
        int n = 0;
        std::uint32_t cp = 0;
        if (b0 >= 0xC2 && b0 <= 0xDF) n = 1, cp = b0 & 0x1F;
        else if (b0 >= 0xE0 && b0 <= 0xEF) n = 2, cp = b0 & 0x0F;
        else if (b0 >= 0xF0 && b0 <= 0xF4) n = 3, cp = b0 & 0x07;
        else return false;
        if (i_ + static_cast<std::size_t>(n) >= t_.size()) return false;
        for (int k = 1; k <= n; ++k) {
            const auto b = static_cast<unsigned char>(t_[i_ + static_cast<std::size_t>(k)]);
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        o.append(t_.substr(i_, static_cast<std::size_t>(n) + 1));
        i_ += static_cast<std::size_t>(n) + 1;
        return true;
    }

    Result<std::string> string() {
        if (!eat('"')) return fail("expected a string");
        std::string o;
        while (true) {
            if (i_ >= t_.size()) return fail("unterminated string");
            if (o.size() > lim_.max_string_bytes) return fail("string too long");
            const auto c = static_cast<unsigned char>(t_[i_]);
            if (c == '"') {
                ++i_;
                return o;
            }
            if (c < 0x20) return fail("control character in a string");
            if (c == '\\') {
                ++i_;
                if (i_ >= t_.size()) return fail("unterminated escape");
                const char e = t_[i_++];
                switch (e) {
                    case '"': o += '"'; break;
                    case '\\': o += '\\'; break;
                    case '/': o += '/'; break;
                    case 'b': case 'f': case 'n': case 'r': case 't':
                        return fail("control character in a string");
                    case 'u': {
                        auto cp = hex4();
                        if (!cp) return fail("bad \\u escape");
                        if (*cp >= 0xD800 && *cp <= 0xDBFF) {
                            if (!(eat('\\') && eat('u'))) return fail("unpaired surrogate");
                            auto lo = hex4();
                            if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return fail("unpaired surrogate");
                            *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
                        } else if (*cp >= 0xDC00 && *cp <= 0xDFFF) {
                            return fail("unpaired surrogate");
                        }
                        if (*cp < 0x20 || *cp == 0x7F) return fail("control character in a string");
                        put_utf8(o, *cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
                continue;
            }
            if (c == 0x7F) return fail("control character in a string");
            if (c < 0x80) {
                o += static_cast<char>(c);
                ++i_;
                continue;
            }
            if (!utf8(o)) return fail("invalid UTF-8");
        }
    }

    std::string_view t_;
    const JsonLimits& lim_;
    std::size_t i_{0};
};

}  // namespace

Result<JsonObject> parse_flat_object(std::string_view text, const JsonLimits& lim) {
    if (text.size() > lim.max_bytes) return Error{Errc::invalid_argument, "invalid JSON: body too large", 0};
    return Parser(text, lim).object();
}

}  // namespace osie::api
