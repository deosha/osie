// SPDX-License-Identifier: Apache-2.0
// Configuration of the opt-in background agent (osie-agent, docs/AGENT.md).
//
// The format is "key = value" lines with '#' comments. Parsing is strict:
// unknown keys, duplicates and out-of-range values are errors, so a typo can
// never silently change behaviour. Persistence, network listeners and remote
// access do not exist in this version; keys that would enable them are
// rejected with an explicit message.
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "osie/core/observation.hpp"
#include "osie/core/result.hpp"
#include "osie/platform/os.hpp"

namespace osie::agent {

enum class LogLevel : std::uint8_t { error, warn, info, debug };
std::string_view to_string(LogLevel l) noexcept;

struct AgentConfig {
    /// Time between the starts of two collections. [250 ms, 1 h]
    Millis interval{5000};
    /// Window over which rates (CPU, per-process CPU) are measured. [50 ms, min(5 s, interval / 2)]
    Millis sample_window{500};
    /// Points kept in the in-memory history. [8, 100000]
    std::size_t history_capacity{720};
    /// Processes kept per history point (top by CPU and by memory). [1, 64]
    std::size_t history_top_k{16};
    /// Linux: cgroups inspected per collection (of the busiest processes). [0, 64]
    std::size_t cgroup_top_n{8};
    /// Consecutive failed collections of a kind before the agent reports itself degraded. [1, 100]
    std::size_t failure_threshold{3};
    /// What to observe; names as in ObservationKind (cpu_times, memory, ...).
    std::vector<ObservationKind> observe{ObservationKind::cpu_times, ObservationKind::load_average,
                                        ObservationKind::memory, ObservationKind::swap,
                                        ObservationKind::memory_pressure_level, ObservationKind::process_list,
                                        ObservationKind::pressure_stall, ObservationKind::cgroup};
    LogLevel log_level{LogLevel::info};
    /// Write a one-line health summary at this interval; 0 = never. [0, 1 day]
    Millis health_log_interval{Millis{0}};
};

/// Parses configuration text. Values: durations take a unit ("500ms", "5s",
/// "2m", "1h"); counts are plain integers; `observe` is a comma-separated list.
Result<AgentConfig> parse_agent_config(std::string_view text);

/// Loads and parses a file. Refuses files over 64 KiB, non-regular files,
/// and files writable by users other than the owner (the configuration
/// decides what the agent observes).
Result<AgentConfig> load_agent_config(const std::string& path);

/// "key = value" rendering of a configuration (round-trips through parse).
std::string to_text(const AgentConfig& c);

}  // namespace osie::agent
