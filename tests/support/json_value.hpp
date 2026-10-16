// SPDX-License-Identifier: Apache-2.0
// A small JSON reader for test assertions only (not used by OSIE itself).
// Throws std::runtime_error on malformed input, so a test fails loudly.
#pragma once

#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace osie::test {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v{nullptr};

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<std::shared_ptr<Object>>(v); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<std::shared_ptr<Array>>(v); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(v); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(v); }
    [[nodiscard]] const Object& obj() const { return *std::get<std::shared_ptr<Object>>(v); }
    [[nodiscard]] const Array& arr() const { return *std::get<std::shared_ptr<Array>>(v); }
    [[nodiscard]] const std::string& str() const { return std::get<std::string>(v); }
    [[nodiscard]] double num() const { return std::get<double>(v); }
    [[nodiscard]] bool boolean() const { return std::get<bool>(v); }
    [[nodiscard]] bool has(const std::string& k) const { return is_object() && obj().count(k) != 0; }
    const JsonValue& operator[](const std::string& k) const {
        if (!has(k)) throw std::runtime_error("missing key: " + k);
        return obj().at(k);
    }
    const JsonValue& operator[](std::size_t i) const { return arr().at(i); }
};

class JsonReader {
public:
    static JsonValue parse(std::string_view t) {
        JsonReader r(t);
        JsonValue v = r.value();
        r.ws();
        if (r.i_ != t.size()) throw std::runtime_error("trailing data");
        return v;
    }

private:
    explicit JsonReader(std::string_view t) : t_(t) {}
    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\t' || t_[i_] == '\r')) ++i_;
    }
    char peek() {
        ws();
        if (i_ >= t_.size()) throw std::runtime_error("unexpected end");
        return t_[i_];
    }
    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::string("expected ") + c);
        ++i_;
    }
    JsonValue value() {
        const char c = peek();
        JsonValue v;
        if (c == '{') {
            ++i_;
            auto o = std::make_shared<JsonValue::Object>();
            if (peek() == '}') {
                ++i_;
            } else {
                while (true) {
                    std::string k = string();
                    expect(':');
                    (*o)[k] = value();
                    if (peek() == ',') { ++i_; continue; }
                    expect('}');
                    break;
                }
            }
            v.v = o;
        } else if (c == '[') {
            ++i_;
            auto a = std::make_shared<JsonValue::Array>();
            if (peek() == ']') {
                ++i_;
            } else {
                while (true) {
                    a->push_back(value());
                    if (peek() == ',') { ++i_; continue; }
                    expect(']');
                    break;
                }
            }
            v.v = a;
        } else if (c == '"') {
            v.v = string();
        } else if (t_.substr(i_, 4) == "true") {
            i_ += 4;
            v.v = true;
        } else if (t_.substr(i_, 5) == "false") {
            i_ += 5;
            v.v = false;
        } else if (t_.substr(i_, 4) == "null") {
            i_ += 4;
        } else {
            const std::string rest(t_.substr(i_, 64));
            char* end = nullptr;
            const double d = std::strtod(rest.c_str(), &end);
            if (end == rest.c_str()) throw std::runtime_error("bad value");
            i_ += static_cast<std::size_t>(end - rest.c_str());
            v.v = d;
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string o;
        while (i_ < t_.size() && t_[i_] != '"') {
            if (t_[i_] == '\\') {
                ++i_;
                const char e = t_.at(i_++);
                if (e == 'u') {
                    const unsigned cp = static_cast<unsigned>(std::stoul(std::string(t_.substr(i_, 4)), nullptr, 16));
                    i_ += 4;
                    if (cp < 0x80) o += static_cast<char>(cp);
                    else o += '?';  // assertions here never need non-ASCII escapes decoded
                } else {
                    o += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
                }
            } else {
                o += t_[i_++];
            }
        }
        expect('"');
        return o;
    }
    std::string_view t_;
    std::size_t i_{0};
};

}  // namespace osie::test
