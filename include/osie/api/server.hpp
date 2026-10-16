// SPDX-License-Identifier: Apache-2.0
// The local API's Unix-domain-socket server (docs/API.md).
//
// Socket. Created in a directory that the walk in osie/util/secure_path
// validates on descriptors: owned by the agent's user (or root) and writable
// by nobody else, with every ancestor trusted. A flock on that directory
// keeps two agents from sharing it. An existing entry is removed only if it
// is a socket owned by the agent's user that nothing answers on (a stale
// socket from a crash); anything else is refused, never deleted. Mode 0600,
// or 0660 owned by api_group; permissions are set before listen(), so there
// is no window with looser ones. On stop the socket is removed only if it is
// still the inode this server created.
//
// Threads. One acceptor and api_max_clients workers. The acceptor reads the
// peer's kernel credentials, decides its access tier, applies a per-uid rate
// limit and a bounded queue (api_max_clients waiting connections), and
// answers refusals itself (401, 429, 503) without reading the request.
// Workers read one request each within api_request_timeout, answer it, and
// close the connection.
//
// Shutdown. stop() closes the listener first, answers queued connections
// with 503 "stopping", lets in-flight requests finish (each is bounded by
// its deadline), and joins every thread. The server holds the runtime by
// shared_ptr, and every handler runs while the server is alive, so no call
// into the runtime can outlive it.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "osie/agent/config.hpp"
#include "osie/agent/runtime.hpp"
#include "osie/api/api.hpp"
#include "osie/util/secure_path.hpp"

namespace osie::api {

/// Percentiles of one timing over the most recent requests (up to 4096).
struct TimingSummary {
    std::size_t n{0};
    double p50{0}, p95{0}, p99{0}, max{0};
};

struct ServerStats {
    std::uint64_t accepted{0}, handled{0};
    std::uint64_t rejected_unauthorized{0}, rejected_rate_limited{0}, rejected_busy{0}, rejected_stopping{0};
    std::uint64_t rejected_per_user{0}, rejected_deadline{0};
    // Recent requests, in milliseconds: accept -> a worker starts it; waiting
    // for the engine and inside it (diagnoses only); accept -> response sent.
    TimingSummary queue_wait_ms, engine_wait_ms, engine_ms, total_ms;
    std::uint64_t timeouts{0}, client_gone{0}, responses_2xx{0}, responses_4xx{0}, responses_5xx{0};
    std::uint64_t in_flight{0}, max_in_flight{0}, queued{0}, max_queued{0};
};

class ApiServer {
public:
    using Logger = std::function<void(agent::LogLevel, const std::string&)>;
    ApiServer(std::shared_ptr<agent::AgentRuntime> rt, const agent::AgentConfig& cfg, Logger log = {});
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    /// Creates the socket and starts the threads. Errors: unsafe or missing
    /// directory, a live or foreign entry at the path, an unknown api_group
    /// or one the agent's user is not a member of.
    Result<void> start();
    /// Graceful stop (see above). Idempotent.
    void stop();

    [[nodiscard]] ServerStats stats() const;
    [[nodiscard]] const std::string& socket_path() const noexcept { return path_; }
    [[nodiscard]] DiagnosticApi& api() noexcept { return *api_; }
    [[nodiscard]] const DiagnosticApi& api() const noexcept { return *api_; }

private:
    struct Conn {
        int fd{-1};
        PeerCredentials peer;
        Access access{Access::none};
        std::chrono::steady_clock::time_point accepted{};
    };
    Result<void> open_socket();
    void close_socket();
    void accept_loop();
    void worker_loop();
    void serve(Conn c);
    bool admit_rate(uid_t uid);
    void quick_reply(int fd, const HttpResponse& r);
    void note_status(int status);
    void log_denial(const std::string& m);

    std::shared_ptr<agent::AgentRuntime> rt_;
    agent::AgentConfig cfg_;
    Logger log_;
    std::string path_, dir_, name_;
    std::unique_ptr<DiagnosticApi> api_;
    AccessPolicy policy_;

    secure::Fd dir_fd_;     // the socket directory; holds the flock
    secure::Fd listen_fd_;
    int wake_r_{-1}, wake_w_{-1};
    std::uint64_t sock_dev_{0}, sock_ino_{0};
    bool socket_created_{false};

    std::mutex run_mu_;  // serialises start/stop
    bool started_{false};
    std::atomic<bool> stopping_{false};
    CancellationSource cancel_;
    std::thread acceptor_;
    std::vector<std::thread> workers_;

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Conn> queue_;
    std::map<uid_t, std::size_t> per_uid_;  // waiting or in progress, by uid (guarded by q_mu_)
    void release_uid(uid_t uid);

    std::mutex rate_mu_;
    struct Bucket {
        double tokens{0};
        std::chrono::steady_clock::time_point at{};
    };
    std::map<uid_t, Bucket> buckets_;

    std::mutex log_mu_;
    std::chrono::steady_clock::time_point log_window_{};
    int log_count_{0};
    std::uint64_t log_suppressed_{0};

    mutable std::mutex stats_mu_;
    ServerStats stats_;
    struct Sample {
        float queue_ms, engine_wait_ms, engine_ms, total_ms;
    };
    std::vector<Sample> samples_;  // ring of the most recent requests (guarded by stats_mu_)
    std::size_t sample_next_{0};
};

}  // namespace osie::api
