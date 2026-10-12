// SPDX-License-Identifier: Apache-2.0
// Minimal streaming JSON writer (no dependency). Produces compact, valid JSON;
// commas are tracked automatically.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace osie::json {

std::string escape(std::string_view s);

class Writer {
public:
    Writer& begin_object() { pre_value(); out_ += '{'; stack_.push_back(true); return *this; }
    Writer& end_object() { stack_.pop_back(); out_ += '}'; return *this; }
    Writer& begin_array() { pre_value(); out_ += '['; stack_.push_back(true); return *this; }
    Writer& end_array() { stack_.pop_back(); out_ += ']'; return *this; }

    Writer& key(std::string_view k) {
        comma();
        out_ += '"';
        out_ += escape(k);
        out_ += "\":";
        after_key_ = true;
        return *this;
    }
    Writer& value(std::string_view s) { pre_value(); out_ += '"'; out_ += escape(s); out_ += '"'; return *this; }
    Writer& value(const char* s) { return value(std::string_view{s}); }
    Writer& value(const std::string& s) { return value(std::string_view{s}); }
    Writer& value(bool b) { pre_value(); out_ += b ? "true" : "false"; return *this; }
    // One template for every integer type: int64_t is `long` on LP64 Linux but
    // `long long` on macOS, so fixed overload sets are not portable.
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Writer& value(T v) { pre_value(); out_ += std::to_string(v); return *this; }
    Writer& value(double d);
    Writer& null() { pre_value(); out_ += "null"; return *this; }

    template <class T>
    Writer& value(const std::optional<T>& o) { return o ? value(*o) : null(); }

    template <class T>
    Writer& kv(std::string_view k, const T& v) { key(k); return value(v); }

    [[nodiscard]] const std::string& str() const noexcept { return out_; }
    std::string take() { return std::move(out_); }

private:
    void comma() {
        if (stack_.empty()) return;
        if (!stack_.back()) out_ += ',';
        stack_.back() = false;
    }
    void pre_value() {
        if (after_key_) { after_key_ = false; return; }
        comma();
    }
    std::string out_;
    std::vector<bool> stack_;  // true = container still empty
    bool after_key_{false};
};

}  // namespace osie::json
