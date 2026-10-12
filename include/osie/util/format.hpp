// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace osie::fmt {

inline std::string bytes(std::uint64_t b) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

inline std::string num(double v, int prec = 1) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    return buf;
}

inline std::string pct(double v, int prec = 1) { return num(v, prec) + "%"; }

}  // namespace osie::fmt
