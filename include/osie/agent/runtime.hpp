// SPDX-License-Identifier: Apache-2.0
// The OSIE agent runtime: an opt-in background observer (docs/AGENT.md).
//
// One scheduler thread collects observations at a fixed cadence through the
// same ObservationBroker, capability registry and read-only policy as the
// CLI, keeps a bounded in-memory history, and publishes each result as an
// immutable snapshot. Readers never wait for a collection: they copy a
// shared_ptr under a lock held for a pointer swap only.
//
// Guarantees:
// - collections never overlap (one scheduler thread; collect_now() refuses
//   while one is running), and there are no unbounded queues;
// - intervals are scheduled on the monotonic clock; reported times are wall
//   clock;
// - all memory is bounded by the configuration (history capacity x top-k
//   processes, cgroups per collection, a fixed-size jitter ring);
// - nothing is written to disk and no network socket is opened;
// - there is no action path: the runtime holds no executor and the policy
//   denies every act.* capability.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "osie/agent/config.hpp"
#include "osie/capabilities/capability.hpp"
#include "osie/state/history.hpp"
#include "osie/state/state.hpp"

namespace osie::agent {

enum class AgentState : std::uint8_t { stopped, starting, running, degraded, stopping };
std::string_view to_string(AgentState s) noexcept;

/// Per observation kind: is it supported here, allowed by policy, and how
/// have recent collections gone.
struct KindHealth {
    ObservationKind kind{};
    bool supported{false};
    bool allowed{false};
    std::uint64_t ok{0}, failed{0};
    std::uint32_t consecutive_failures{0};
    std::string last_error;
    std::optional<WallTime> last_ok;
};

struct TimingStats {
    std::size_t n{0};
    double p50_ms{0}, p99_ms{0}, max_ms{0};
};

struct HealthStatus {
    AgentState state{AgentState::stopped};
    std::optional<WallTime> started_at;
    std::uint64_t collections{0};          // completed collections
    std::uint64_t failed_collections{0};   // collections where a requested kind failed
    std::uint64_t skipped_ticks{0};        // ticks missed because a collection overran
    std::uint64_t rejected_manual{0};      // collect_now() calls refused while busy
    std::optional<WallTime> last_collection_wall;
    std::optional<double> last_collection_age_s;  // monotonic age of the newest snapshot
    double last_collection_ms{0};          // duration of the newest collection
    TimingStats start_jitter;              // actual start - scheduled start
    std::size_t history_points{0};
    std::size_t history_bytes{0};
    std::vector<KindHealth> kinds;
    std::vector<std::string> degraded_reasons;
};

/// One published collection. Immutable once published.
struct AgentSnapshot {
    std::uint64_t seq{0};
    WallTime wall{};
    SteadyTime mono{};
    std::shared_ptr<const SystemState> state;
};

class AgentRuntime {
public:
    AgentRuntime(std::unique_ptr<IOperatingSystem> os, AgentConfig cfg);
    ~AgentRuntime();
    AgentRuntime(const AgentRuntime&) = delete;
    AgentRuntime& operator=(const AgentRuntime&) = delete;

    /// Starts the scheduler thread. Fails if already running.
    Result<void> start();
    /// Stops the scheduler: cancels an in-flight collection and joins the
    /// thread. Idempotent; safe to call from any thread except the scheduler.
    void stop();
    [[nodiscard]] bool running() const noexcept;

    /// Runs one collection on the calling thread, unless one is already in
    /// progress (then returns false and nothing is collected).
    bool collect_now();

    /// Applies a new configuration between collections (history keeps its
    /// points; capacity changes trim the oldest). Invalid combinations are
    /// rejected by the config parser, not here.
    void reconfigure(const AgentConfig& cfg);

    [[nodiscard]] std::shared_ptr<const AgentSnapshot> snapshot() const;
    [[nodiscard]] HealthStatus health() const;
    /// Newest `max` history points (copied under a short lock).
    [[nodiscard]] std::vector<HistoryPoint> history(std::size_t max) const;
    [[nodiscard]] const CapabilityRegistry& registry() const noexcept { return reg_; }
    [[nodiscard]] const AuditLog& audit() const noexcept { return audit_; }
    [[nodiscard]] AgentConfig config() const;
    [[nodiscard]] Platform platform() const noexcept { return os_->platform(); }
    [[nodiscard]] ObservationSet supported() const { return os_->supported(); }

private:
    void loop();
    void collect(const CancellationToken& cancel);
    void record_jitter(double ms);

    std::unique_ptr<IOperatingSystem> os_;
    CapabilityRegistry reg_;
    SafetyPolicy policy_;
    mutable AuditLog audit_;
    ObservationBroker broker_;
    StateSampler sampler_;

    mutable std::mutex cfg_mu_;
    AgentConfig cfg_;

    mutable std::mutex snap_mu_;  // guards the snapshot pointer only
    std::shared_ptr<const AgentSnapshot> snap_;

    mutable std::mutex hist_mu_;  // guards history_ and health counters
    StateHistory history_;
    HealthStatus health_;
    std::array<double, 1024> jitter_{};
    std::size_t jitter_n_{0};

    std::atomic<bool> collecting_{false};
    std::atomic<AgentState> state_{AgentState::stopped};
    std::mutex run_mu_;  // serialises start/stop
    std::mutex wake_mu_;
    std::condition_variable wake_;
    bool stop_requested_{false};
    std::unique_ptr<CancellationSource> cancel_;
    std::thread thread_;
    std::uint64_t seq_{0};
};

std::string to_json(const HealthStatus& h);
/// Capability and policy status: every capability, whether it is supported on
/// this platform and allowed by the read-only policy.
std::string capabilities_json(const AgentRuntime& r);

}  // namespace osie::agent
