// SPDX-License-Identifier: Apache-2.0
#include "osie/util/json.hpp"

#include <cstdio>

namespace osie::json {

std::string escape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

Writer& Writer::value(double d) {
    pre_value();
    if (!std::isfinite(d)) {
        out_ += "null";
        return *this;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", d);
    out_ += buf;
    return *this;
}

}  // namespace osie::json
