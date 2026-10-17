// SPDX-License-Identifier: Apache-2.0
// The local diagnostic API's endpoints (docs/API.md), independent of the
// transport: an HttpRequest and the peer's kernel identity in, an
// HttpResponse out. The socket server (server.hpp) adds framing, deadlines
// and admission control around it.
//
// Every answer is built from the agent's published evidence: the API owns an
// Engine whose operating-system backend refuses all observation (RefusingOs),
// and gives it the agent's snapshot and history through AgentEvidence. There
// is no second sampler, scheduler or history.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "osie/agent/runtime.hpp"
#include "osie/api/evidence.hpp"
#include "osie/api/http.hpp"
#include "osie/api/peer.hpp"
#include "osie/api/sessions.hpp"
#include "osie/engine/engine.hpp"

namespace osie::api {

inline constexpr const char* kSchemaVersion = "1.0";

struct ApiSettings {
    Millis max_evidence_age{10'000};  // cap for every answer; clients may ask for less
    std::size_t max_body_bytes{16 * 1024};
    std::size_t max_question_bytes{2048};
    std::size_t max_sessions{128};
    Millis session_ttl{600'000};
    ProcVisibility visibility{ProcVisibility::all};
    // Reported by /v1/capabilities.
    std::size_t max_clients{16};
    std::size_t max_clients_per_user{4};
    Millis request_timeout{5000};
    std::size_t rate_limit{50};
};

/// The automatic evidence age limit: 2 x interval + sample window + 2 s.
Millis automatic_max_evidence_age(const agent::AgentConfig& c);
/// Consecutive history points further apart than this are a gap:
/// 2 x interval + sample window, in seconds.
double history_gap_threshold_s(const agent::AgentConfig& c);
/// ApiSettings from the agent configuration.
ApiSettings settings_from(const agent::AgentConfig& c);

class DiagnosticApi {
public:
    /// Holds a reference to the runtime for its whole life (the lifetime
    /// contract of AgentRuntime: every call it makes happens while this
    /// shared_ptr is held).
    DiagnosticApi(std::shared_ptr<agent::AgentRuntime> rt, ApiSettings s);
    DiagnosticApi(const DiagnosticApi&) = delete;
    DiagnosticApi& operator=(const DiagnosticApi&) = delete;

    /// Routes and answers one request. `access` was decided from the peer's
    /// credentials before the request was read. A diagnosis that cannot get
    /// the engine before `deadline` is refused (503 busy) rather than waiting
    /// past it. Thread-safe.
    HttpResponse handle(const HttpRequest& req, const PeerCredentials& peer, Access access,
                        const CancellationToken& cancel = {}, SteadyTime deadline = SteadyTime::max());
    [[nodiscard]] const Engine& engine() const noexcept { return engine_; }

    /// Readiness then reports "stopping".
    void set_stopping() noexcept { stopping_ = true; }

    /// An error response in the API's envelope.
    static HttpResponse error_response(const ApiError& e, const std::string& request_id = {});

    [[nodiscard]] const RefusingOs& os() const noexcept { return *os_; }
    [[nodiscard]] const SessionStore& sessions() const noexcept { return sessions_; }
    [[nodiscard]] const ApiSettings& settings() const noexcept { return s_; }

private:
    HttpResponse live(const std::string& rid);
    HttpResponse ready(const std::string& rid);
    HttpResponse capabilities(const std::string& rid);
    HttpResponse snapshot(const HttpRequest& req, const PeerCredentials& peer, Access access, const std::string& rid);
    HttpResponse history(const HttpRequest& req, const PeerCredentials& peer, Access access, const std::string& rid);
    HttpResponse diagnose(const HttpRequest& req, const PeerCredentials& peer, Access access, const std::string& rid,
                          const CancellationToken& cancel, SteadyTime deadline);

    std::shared_ptr<agent::AgentRuntime> rt_;
    ApiSettings s_;
    RefusingOs* os_;  // owned by engine_
    Engine engine_;
    SessionStore sessions_;
    std::atomic<bool> stopping_{false};
    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

/// "2026-10-16T10:00:00.123Z".
std::string iso8601(WallTime t);

}  // namespace osie::api
