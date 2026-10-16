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
// - collections never overlap (one admission slot shared by the scheduler and
//   collect_now()), and there are no unbounded queues;
// - stop() cancels and awaits the in-flight collection, so no collection
//   touches the backend once it has returned;
// - destruction must be ordered after other threads' calls by the owner
//   (see the lifetime contract below); the runtime cannot make concurrent
//   destruction safe;
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

namespace osie::json {
class Writer;
}

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

/// Lifetime, ownership and concurrency contract.
///
/// Lifetime (the caller's obligation). An AgentRuntime must outlive every
/// call to its members. Its destructor may begin only after every member
/// call made by any other thread has returned, and no call may begin once
/// it has started. Destroying the runtime concurrently with a member call
/// on another thread is undefined behaviour. Nothing inside the runtime can
/// prevent that: a thread that is still entering a member function may touch
/// a mutex or a field that the destructor is already freeing. Internal
/// cancellation does not change this. The owner must order destruction
/// after the other threads' calls, either:
///   - stop(), then join (or otherwise synchronise with) every thread that
///     calls the runtime, then destroy it; or
///   - share ownership: give each calling thread a std::shared_ptr to the
///     runtime for as long as it makes calls, so that the destructor runs
///     after the last of them has dropped its reference.
/// stop() returning does not mean that other threads have left their member
/// calls (a cancelled collect_now() may still be returning), so it does not
/// on its own make destruction safe. References returned by registry() and
/// audit() follow the same rule.
///
/// What the runtime owns. The operating-system backend, the broker, the
/// sampler, the policy, the audit log, the history and the scheduler thread.
/// The destructor stops and joins the scheduler thread, which is internal, so
/// destroying a running runtime is safe as far as that thread is concerned.
/// Published snapshots own their data (shared_ptr), so a reader may keep one
/// after the runtime has been destroyed. history() and health() return
/// copies.
///
/// Thread safety. While the runtime is alive, every public member may be
/// called from any thread, concurrently with every other member, including
/// stop(). The destructor is not a member call: see Lifetime. The scheduler
/// thread never calls stop() and never runs the destructor.
///
/// Collections. At most one collection runs at a time, whether started by the
/// scheduler or by collect_now(). Each has its own cancellation source. A
/// collection is "in flight" from the moment it is admitted until it has
/// finished touching the backend and the history.
///
/// Shutdown. stop():
///   1. closes admission: until it returns, collect_now() is refused and the
///      scheduler starts nothing;
///   2. cancels the in-flight collection, scheduled or manual;
///   3. joins the scheduler;
///   4. waits until no collection is in flight.
/// After stop() returns, the runtime may be started again or used for manual
/// collections.
///
/// Reconfiguration. reconfigure() is serialised with itself and applied
/// atomically with respect to readers: the configuration, the history bounds
/// and the per-kind health always correspond to the same call. A collection
/// already in flight finishes with the configuration it started with.
class AgentRuntime {
public:
    AgentRuntime(std::unique_ptr<IOperatingSystem> os, AgentConfig cfg);
    /// Precondition: no member call is in progress on another thread, and none
    /// will begin (see Lifetime). Stops and joins the scheduler thread.
    ~AgentRuntime();
    AgentRuntime(const AgentRuntime&) = delete;
    AgentRuntime& operator=(const AgentRuntime&) = delete;

    /// Starts the scheduler thread. Fails if already running.
    Result<void> start();
    /// Stops the scheduler and any manual collection (see the contract above).
    /// Idempotent; returns once no collection is in flight.
    void stop();
    [[nodiscard]] bool running() const noexcept;

    /// Runs one collection on the calling thread. Returns true if it was
    /// completed and published; false if it was refused (another collection
    /// is in flight, or shutdown has begun) or cancelled by stop().
    bool collect_now();

    /// Applies a new configuration between collections. The history is trimmed
    /// to the new bounds immediately: the oldest points beyond the capacity are
    /// dropped and every kept point is cut to the new top-k. Invalid
    /// combinations are rejected by the config parser, not here.
    void reconfigure(const AgentConfig& cfg);

    [[nodiscard]] std::shared_ptr<const AgentSnapshot> snapshot() const;
    /// The newest snapshot and the one published before it (either may be
    /// null), read together: rates between two collections, such as cgroup
    /// throttling, are derived from them without sampling again.
    [[nodiscard]] std::pair<std::shared_ptr<const AgentSnapshot>, std::shared_ptr<const AgentSnapshot>> snapshots() const;
    /// A copy of the whole history, with its bounds (copied under a short lock).
    [[nodiscard]] StateHistory history_copy() const;
    [[nodiscard]] HealthStatus health() const;
    /// Newest `max` history points (copied under a short lock).
    [[nodiscard]] std::vector<HistoryPoint> history(std::size_t max) const;
    [[nodiscard]] const CapabilityRegistry& registry() const noexcept { return reg_; }
    [[nodiscard]] const AuditLog& audit() const noexcept { return audit_; }
    [[nodiscard]] AgentConfig config() const;
    [[nodiscard]] Platform platform() const noexcept { return os_->platform(); }
    [[nodiscard]] ObservationSet supported() const { return os_->supported(); }

private:
    enum class Admit : std::uint8_t { ok, busy, closed };
    void loop();
    /// Admits one collection and hands out its cancellation token.
    Admit begin_collection(CancellationToken& token);
    void end_collection();
    /// Returns true if the collection was completed and published.
    bool collect(const CancellationToken& cancel);
    void record_jitter(double ms);
    /// Moves state_ to `to` only while the scheduler is live (starting, running, degraded).
    void set_live_state(AgentState to);

    std::unique_ptr<IOperatingSystem> os_;
    CapabilityRegistry reg_;
    SafetyPolicy policy_;
    mutable AuditLog audit_;
    ObservationBroker broker_;
    StateSampler sampler_;

    // Lock order: run_mu_ -> life_mu_; cfg_mu_ -> hist_mu_. snap_mu_ is a leaf.
    mutable std::mutex cfg_mu_;  // guards cfg_; held for the whole of reconfigure()
    AgentConfig cfg_;

    mutable std::mutex snap_mu_;  // guards the snapshot pointers only
    std::shared_ptr<const AgentSnapshot> snap_;
    std::shared_ptr<const AgentSnapshot> prev_snap_;
    std::uint64_t seq_{0};

    mutable std::mutex hist_mu_;  // guards history_, health_ and the jitter ring
    StateHistory history_;
    HealthStatus health_;
    std::array<double, 1024> jitter_{};
    std::size_t jitter_n_{0};

    std::atomic<AgentState> state_{AgentState::stopped};
    std::mutex run_mu_;  // serialises start/stop
    std::mutex life_mu_;  // guards the fields below
    std::condition_variable wake_;  // scheduler: stop requested
    std::condition_variable idle_;  // stop(): the in-flight collection has finished
    bool stop_requested_{false};
    bool closed_{false};  // admission closed: stop() in progress
    bool in_flight_{false};
    CancellationSource active_;  // the in-flight collection's source
    std::thread thread_;
};

std::string to_json(const HealthStatus& h);
/// Capability and policy status: every capability, whether it is supported on
/// this platform and allowed by the read-only policy.
std::string capabilities_json(const AgentRuntime& r);
/// Writes the same array into `w`.
void write_capabilities(json::Writer& w, const AgentRuntime& r);

}  // namespace osie::agent
