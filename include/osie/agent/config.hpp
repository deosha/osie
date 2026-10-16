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

    // ---- Local diagnostic API (docs/API.md): off unless enabled. ----------
    // Read at start only; a reload that changes these keeps the running values.
    /// Serve the API on a Unix domain socket. There is never an IP listener.
    bool api_enabled{false};
    /// Socket path; its directory must belong to the agent's user and be
    /// writable by nobody else (systemd: RuntimeDirectory=osie).
    std::string api_socket{"/run/osie/agent.sock"};
    /// Members of this group may use the diagnostic endpoints (socket mode
    /// 0660, group-owned). Empty: only root and the agent's own user (0600).
    std::string api_group;
    /// Requests handled at once; as many more may wait. [1, 64]
    std::size_t api_max_clients{16};
    /// Requests one user (uid) may have waiting or in progress, so that one
    /// user cannot occupy every worker. [1, api_max_clients]
    std::size_t api_max_clients_per_user{4};
    /// Largest request (headers and body). [1 KiB, 1 MiB]
    std::size_t api_max_request_bytes{16 * 1024};
    /// Deadline for reading, answering and writing one request. [100 ms, 60 s]
    Millis api_request_timeout{5000};
    /// Conversation sessions kept in memory. [1, 4096]
    std::size_t api_max_sessions{128};
    /// A session unused for this long is forgotten. [10 s, 1 day]
    Millis api_session_ttl{600'000};
    /// Evidence older than this is never used for a diagnosis; 0 = automatic
    /// (2 x interval + sample_window + 2 s). [0, 1 h]
    Millis api_max_evidence_age{0};
    /// Requests per second per client user (burst: twice this). [1, 10000]
    std::size_t api_rate_limit{50};
};

/// Parses configuration text. Values: durations take a unit ("500ms", "5s",
/// "2m", "1h"); counts are plain integers; `observe` is a comma-separated list.
Result<AgentConfig> parse_agent_config(std::string_view text);

/// Loads and parses a file. The configuration decides what the agent
/// observes, so the file and everything that could replace it must be under
/// the control of root or the agent's own user. Refused:
/// - files over 64 KiB (counted on the bytes read), and non-regular files
///   (directories, FIFOs, devices; a FIFO never blocks the loader);
/// - a file owned by another user, or writable by group or others;
/// - a path through a directory owned by another user, or writable by group
///   or others without the sticky bit;
/// - a symbolic link owned by another user (links owned by root or the
///   agent's user are followed, and their targets checked the same way).
/// Each directory is opened relative to the previous one with O_NOFOLLOW and
/// validated on its descriptor, and the file is validated with fstat() on
/// the descriptor that is read, so swapping a path component during the load
/// cannot substitute an unchecked file.
Result<AgentConfig> load_agent_config(const std::string& path);

/// "key = value" rendering of a configuration (round-trips through parse).
std::string to_text(const AgentConfig& c);

}  // namespace osie::agent
