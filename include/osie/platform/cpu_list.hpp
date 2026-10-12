// SPDX-License-Identifier: Apache-2.0
// Parser for the kernel's CPU list format ("0-3,8,10-11"), used by
// cpuset.cpus.effective and /sys/devices/system/cpu/online.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace osie {

/// Number of CPUs in a list such as "0-3,8". nullopt on malformed input
/// (ranges are bounded to keep hostile files from causing large loops).
inline std::optional<std::uint32_t> parse_cpu_list_count(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    if (s.empty()) return std::nullopt;
    std::uint32_t total = 0;
    while (!s.empty()) {
        const auto comma = s.find(',');
        const std::string_view part = s.substr(0, comma);
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
        auto num = [](std::string_view t) -> std::optional<std::uint32_t> {
            if (t.empty() || t.size() > 5) return std::nullopt;
            std::uint32_t v = 0;
            for (char c : t) {
                if (c < '0' || c > '9') return std::nullopt;
                v = v * 10 + static_cast<std::uint32_t>(c - '0');
            }
            return v;
        };
        const auto dash = part.find('-');
        if (dash == std::string_view::npos) {
            if (!num(part)) return std::nullopt;
            ++total;
        } else {
            auto a = num(part.substr(0, dash)), b = num(part.substr(dash + 1));
            if (!a || !b || *b < *a) return std::nullopt;
            total += *b - *a + 1;
        }
        if (total > 65536) return std::nullopt;
    }
    return total;
}

}  // namespace osie
