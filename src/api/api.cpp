// SPDX-License-Identifier: Apache-2.0
#include "osie/api/api.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "osie/api/json_in.hpp"
#include "osie/engine/serialize.hpp"
#include "osie/intent/intent.hpp"
#include "osie/util/json.hpp"

namespace osie::api {
namespace {

using intent::Intent;

ApiError make(int status, ErrorCode code, std::string reason, std::string message, bool retryable = false,
              std::optional<int> retry_after = std::nullopt) {
    ApiError e;
    e.status = status;
    e.code = code;
    e.reason = std::move(reason);
    e.message = std::move(message);
    e.retryable = retryable;
    e.retry_after_s = retry_after;
    return e;
}

json::Writer envelope(const std::string& rid) {
    json::Writer w;
    w.begin_object().kv("schema_version", kSchemaVersion);
    if (!rid.empty()) w.kv("request_id", rid);
    return w;
}

HttpResponse ok(json::Writer& w, int status = 200) {
    w.end_object();
    HttpResponse r;
    r.status = status;
    r.body = w.take();
    return r;
}

/// Finite numbers only; NaN or infinity is written as null.
void num(json::Writer& w, std::string_view k, std::optional<double> v) {
    w.key(k);
    if (v && std::isfinite(*v)) w.value(*v);
    else w.null();
}

double ms_age(SteadyTime t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void write_process(json::Writer& w, const ProcessView& p) {
    w.begin_object().kv("pid", static_cast<std::int64_t>(p.info.pid));
    w.key("start_time_us").value(p.info.start_time_us);
    w.kv("name", p.info.name).kv("uid", static_cast<std::int64_t>(p.info.uid));
    num(w, "cpu_pct", p.cpu_pct);
    w.key("memory_bytes").value(p.info.memory_bytes());
    w.key("threads").value(p.info.threads);
    if (!p.info.path.empty()) w.kv("path", p.info.path);  // absent when unknown or withheld
    w.end_object();
}

const char* kMandatory[] = {"cpu_times", "memory", "process_list"};

std::string first_line(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size()) {
        const auto e = s.find('\n', b);
        std::string line = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (line.find_first_not_of(" \t") != std::string::npos) return line;
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return {};
}

bool id_chars(const std::string& v) {
    return !v.empty() && v.size() <= 64 && std::all_of(v.begin(), v.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == ':' || c == '-';
    });
}

struct Limitation {
    std::string code, detail;
};

}  // namespace

std::string iso8601(WallTime t) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[96];  // ample for any int fields: no truncation
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

Millis automatic_max_evidence_age(const agent::AgentConfig& c) {
    return c.interval * 2 + c.sample_window + Millis{2000};
}

ApiSettings settings_from(const agent::AgentConfig& c) {
    ApiSettings s;
    s.max_evidence_age = c.api_max_evidence_age.count() > 0 ? c.api_max_evidence_age : automatic_max_evidence_age(c);
    s.max_body_bytes = c.api_max_request_bytes;
    s.max_sessions = c.api_max_sessions;
    s.session_ttl = c.api_session_ttl;
    s.max_clients = c.api_max_clients;
    s.request_timeout = c.api_request_timeout;
    s.rate_limit = c.api_rate_limit;
    return s;
}

DiagnosticApi::DiagnosticApi(std::shared_ptr<agent::AgentRuntime> rt, ApiSettings s)
    : rt_(std::move(rt)),
      s_(s),
      os_(nullptr),
      engine_(
          [&] {
              auto o = std::make_unique<RefusingOs>(rt_->platform(), rt_->supported());
              os_ = o.get();
              return o;
          }(),
          std::make_unique<intent::RuleParser>(),
          [&] {
              EngineConfig ec;
              ec.context_ttl = s.session_ttl;
              ec.experimental_prediction = false;  // predictions are never offered through the API
              return ec;
          }()),
      sessions_(s.max_sessions, s.session_ttl) {}

HttpResponse DiagnosticApi::error_response(const ApiError& e, const std::string& rid) {
    auto w = envelope(rid);
    w.key("error").begin_object().kv("code", to_string(e.code)).kv("reason", e.reason).kv("message", e.message)
        .kv("retryable", e.retryable).end_object();
    w.kv("action_executed", false);
    HttpResponse r = ok(w, e.status);
    if (e.retry_after_s) r.headers.emplace_back("Retry-After", std::to_string(*e.retry_after_s));
    return r;
}

HttpResponse DiagnosticApi::handle(const HttpRequest& req, const PeerCredentials& peer, Access access,
                                   const CancellationToken& cancel) {
    const std::string& rid = req.request_id;
    if (access == Access::none)
        return error_response(make(401, ErrorCode::unauthorized, "unknown_peer",
                                   "The peer's identity could not be determined."), rid);
    struct Route {
        std::string_view path, method;
        Access min;
    };
    static constexpr Route kRoutes[] = {
        {"/v1/health/live", "GET", Access::health},      {"/v1/health/ready", "GET", Access::health},
        {"/v1/capabilities", "GET", Access::diagnostics}, {"/v1/system/snapshot", "GET", Access::diagnostics},
        {"/v1/system/history", "GET", Access::diagnostics}, {"/v1/diagnose", "POST", Access::diagnostics},
    };
    const auto* route = std::find_if(std::begin(kRoutes), std::end(kRoutes), [&](const Route& r) { return r.path == req.path; });
    if (route == std::end(kRoutes))
        return error_response(make(404, ErrorCode::unsupported, "unknown_endpoint", "No such endpoint."), rid);
    if (req.method != route->method) {
        auto r = error_response(make(405, ErrorCode::invalid_request, "method_not_allowed",
                                     "This endpoint accepts " + std::string(route->method) + " only."), rid);
        r.headers.emplace_back("Allow", std::string(route->method));
        return r;
    }
    if (access < route->min)
        return error_response(make(403, ErrorCode::forbidden, "diagnostics_not_permitted",
                                   "This user may use the health endpoints only. Diagnostics need membership of the "
                                   "agent's API group (api_group)."), rid);
    if (route->method == "GET" && req.content_length && *req.content_length > 0)
        return error_response(make(400, ErrorCode::invalid_request, "unexpected_body", "GET requests take no body."), rid);
    if (!req.query.empty() && req.path != "/v1/system/history")
        return error_response(make(400, ErrorCode::invalid_request, "unexpected_query",
                                   "This endpoint takes no query parameters."), rid);
    if (req.path == "/v1/health/live") return live(rid);
    if (req.path == "/v1/health/ready") return ready(rid);
    if (req.path == "/v1/capabilities") return capabilities(rid);
    if (req.path == "/v1/system/snapshot") return snapshot(req, peer, access, rid);
    if (req.path == "/v1/system/history") return history(req, peer, access, rid);
    return diagnose(req, peer, access, rid, cancel);
}

// ------------------------------------------------------------------ health ---

HttpResponse DiagnosticApi::live(const std::string& rid) {
    const auto state = rt_->health().state;
    const bool alive = rt_->running() || stopping_ || state == agent::AgentState::stopping;
    auto w = envelope(rid);
    w.kv("status", alive ? "alive" : "runtime_stopped").kv("runtime_state", agent::to_string(state))
        .kv("uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count());
    return ok(w, alive ? 200 : 503);
}

HttpResponse DiagnosticApi::ready(const std::string& rid) {
    const auto h = rt_->health();
    const auto snap = rt_->snapshot();
    const auto cfg = rt_->config();
    std::vector<std::string> reasons;
    std::string status;
    bool mandatory_failing = false;
    auto w = envelope(rid);
    if (stopping_ || h.state == agent::AgentState::stopping) {
        status = "stopping";
    } else if (!rt_->running()) {
        status = "unavailable";
        reasons.emplace_back("the agent's collector is not running");
    } else if (!snap) {
        status = "starting";
        reasons.emplace_back("no collection has completed yet");
    } else {
        for (const auto& k : h.kinds) {
            const bool mandatory = std::any_of(std::begin(kMandatory), std::end(kMandatory),
                                               [&](const char* m) { return to_string(k.kind) == m; });
            if (k.supported && k.allowed && k.consecutive_failures >= cfg.failure_threshold) {
                reasons.push_back(std::string(to_string(k.kind)) + ": " + std::to_string(k.consecutive_failures) +
                                  " consecutive failures");
                if (mandatory) mandatory_failing = true;
            }
        }
        const double age = ms_age(snap->mono);
        if (age > static_cast<double>(s_.max_evidence_age.count())) {
            status = "unavailable";
            reasons.insert(reasons.begin(), "the newest evidence is older than " +
                                                std::to_string(s_.max_evidence_age.count()) + " ms");
        } else if (mandatory_failing) {
            status = "unavailable";
        } else {
            status = reasons.empty() ? "ready" : "degraded";
        }
    }
    const bool serving = status == "ready" || status == "degraded";
    w.kv("status", status).kv("runtime_state", agent::to_string(h.state));
    if (snap) num(w, "snapshot_age_ms", ms_age(snap->mono));
    else w.key("snapshot_age_ms").null();
    w.kv("max_evidence_age_ms", static_cast<std::int64_t>(s_.max_evidence_age.count()));
    w.key("mandatory").begin_array();
    for (const char* m : kMandatory) w.value(m);
    w.end_array();
    w.key("reasons").begin_array();
    for (const auto& r : reasons) w.value(r);
    w.end_array();
    HttpResponse r = ok(w, serving ? 200 : 503);
    if (!serving) r.headers.emplace_back("Retry-After", std::to_string(std::max<long long>(1, cfg.interval.count() / 1000)));
    return r;
}

// ------------------------------------------------------------ capabilities ---

HttpResponse DiagnosticApi::capabilities(const std::string& rid) {
    const auto cfg = rt_->config();
    const auto snap = rt_->snapshot();
    const auto sup = rt_->supported();
    auto w = envelope(rid);
    w.kv("platform", rt_->platform() == Platform::linux_os ? "linux" : "macos");
    w.key("endpoints").begin_array();
    for (const char* e : {"GET /v1/health/live", "GET /v1/health/ready", "GET /v1/capabilities", "GET /v1/system/snapshot",
                          "GET /v1/system/history", "POST /v1/diagnose"})
        w.value(e);
    w.end_array();
    w.key("observations").begin_array();
    for (unsigned k = 0; k < static_cast<unsigned>(ObservationKind::count_); ++k) {
        const auto kind = static_cast<ObservationKind>(k);
        const bool observed = std::find(cfg.observe.begin(), cfg.observe.end(), kind) != cfg.observe.end() ||
                              kind == ObservationKind::system_info || kind == ObservationKind::cpu_topology;
        w.begin_object().kv("kind", to_string(kind)).kv("supported", sup.contains(kind))
            .kv("collected_by_agent", observed && sup.contains(kind))
            .kv("in_latest_snapshot", snap && snap->state && snap->state->has(kind)).end_object();
    }
    w.end_array();
    w.key("diagnoses").begin_array();
    for (const char* d : {"top_cpu", "top_memory", "diagnose_cpu", "diagnose_memory", "diagnose_slow", "process_cpu",
                          "process_memory", "process_info", "process_tree", "memory_status", "pressure", "cgroup_status",
                          "container_throttle", "trends", "explain_concept"})
        w.value(d);
    w.end_array();
    w.key("not_offered").begin_array();
    for (const char* d : {"actions (OSIE is read-only)", "numeric predictions", "on-demand observation"}) w.value(d);
    w.end_array();
    w.key("limits").begin_object().kv("max_request_bytes", static_cast<std::uint64_t>(s_.max_body_bytes))
        .kv("max_question_bytes", static_cast<std::uint64_t>(s_.max_question_bytes))
        .kv("request_timeout_ms", static_cast<std::int64_t>(s_.request_timeout.count()))
        .kv("max_concurrent_requests", static_cast<std::uint64_t>(s_.max_clients))
        .kv("requests_per_second_per_user", static_cast<std::uint64_t>(s_.rate_limit))
        .kv("max_evidence_age_ms", static_cast<std::int64_t>(s_.max_evidence_age.count()))
        .kv("max_sessions", static_cast<std::uint64_t>(s_.max_sessions))
        .kv("session_ttl_ms", static_cast<std::int64_t>(s_.session_ttl.count())).end_object();
    w.key("policy");
    agent::write_capabilities(w, *rt_);
    return ok(w);
}

// ---------------------------------------------------------------- snapshot ---

HttpResponse DiagnosticApi::snapshot(const HttpRequest&, const PeerCredentials& peer, Access access,
                                     const std::string& rid) {
    const auto snap = rt_->snapshot();
    if (!snap || !snap->state)
        return error_response(make(503, ErrorCode::insufficient_evidence, "no_snapshot",
                                   "The agent has not completed a collection yet.", true, 1), rid);
    std::shared_ptr<const SystemState> st = snap->state;
    if (access != Access::full) {
        auto copy = std::make_shared<SystemState>(*st);
        redact(*copy, peer.uid, s_.visibility);
        st = copy;
    }
    const auto& s = *st;
    auto w = envelope(rid);
    w.kv("snapshot_seq", snap->seq).kv("collected_at", iso8601(snap->wall));
    num(w, "age_ms", ms_age(snap->mono));
    w.kv("sample_window_ms", static_cast<std::int64_t>(s.interval.count()));
    w.key("host");
    if (s.system || s.topology) {
        w.begin_object();
        if (s.system) w.kv("os", s.system->value.os_name).kv("os_version", s.system->value.os_version)
                          .kv("kernel", s.system->value.kernel_release).kv("arch", s.system->value.arch);
        w.kv("logical_cpus", static_cast<std::uint64_t>(s.logical_cpus()));
        w.end_object();
    } else {
        w.null();
    }
    w.key("cpu");
    if (s.cpu) {
        const auto& c = s.cpu->value;
        w.begin_object();
        num(w, "busy_pct", c.busy_pct());
        num(w, "user_pct", c.user_pct);
        num(w, "system_pct", c.system_pct);
        num(w, "iowait_pct", c.iowait_pct);
        num(w, "steal_pct", c.steal_pct);
        w.end_object();
    } else {
        w.null();
    }
    w.key("load");
    if (s.load) w.begin_object().kv("one", s.load->value.one).kv("five", s.load->value.five).kv("fifteen", s.load->value.fifteen).end_object();
    else w.null();
    w.key("memory");
    if (s.memory) {
        const auto& m = s.memory->value;
        w.begin_object().kv("total_bytes", m.total_bytes);
        w.key("available_bytes").value(m.available_bytes);
        w.key("swap_total_bytes").value(m.swap_total_bytes);
        w.key("swap_used_bytes").value(m.swap_used_bytes);
        w.key("pressure_level");
        if (m.pressure_level) w.value(to_string(*m.pressure_level));
        else w.null();
        w.end_object();
    } else {
        w.null();
    }
    w.key("pressure_stall").begin_array();
    if (s.psi)
        for (const auto& p : s.psi->value) {
            w.begin_object().kv("resource", to_string(p.resource)).kv("some_avg10", p.some.avg10).kv("some_avg60", p.some.avg60);
            if (p.full) w.kv("full_avg10", p.full->avg10);
            w.end_object();
        }
    w.end_array();
    w.key("processes");
    if (s.processes) {
        w.begin_object().kv("count", static_cast<std::uint64_t>(s.processes->value.size()))
            .kv("unreadable", static_cast<std::uint64_t>(s.processes_partial))
            .kv("exited_during_sampling", static_cast<std::uint64_t>(s.processes_vanished));
        w.key("top_cpu").begin_array();
        for (const auto* p : s.top_by_cpu(10)) write_process(w, *p);
        w.end_array();
        w.key("top_memory").begin_array();
        for (const auto* p : s.top_by_memory(10)) write_process(w, *p);
        w.end_array();
        w.end_object();
    } else {
        w.null();
    }
    w.key("cgroups").begin_array();
    for (const auto& c : s.cgroups) {
        w.begin_object().kv("path", c.info.path);
        w.key("cpu_quota_us").value(c.info.cpu_quota_us);
        w.key("cpu_period_us").value(c.info.cpu_period_us);
        num(w, "lifetime_throttled_ratio", c.lifetime_throttled_ratio);
        w.key("memory_current_bytes").value(c.info.memory_current);
        w.key("memory_max_bytes").value(c.info.memory_max);
        w.end_object();
    }
    w.end_array();
    w.key("observations").begin_object().key("present").begin_array();
    for (unsigned k = 0; k < static_cast<unsigned>(ObservationKind::count_); ++k)
        if (s.has(static_cast<ObservationKind>(k))) w.value(to_string(static_cast<ObservationKind>(k)));
    w.end_array().key("unavailable").begin_array();
    for (const auto& u : s.unavailable) serialize::write_unavailable(w, u, false);
    w.end_array().end_object();
    if (access != Access::full) w.kv("redacted", "executable paths of other users' processes are withheld");
    return ok(w);
}

// ----------------------------------------------------------------- history ---

HttpResponse DiagnosticApi::history(const HttpRequest& req, const PeerCredentials&, Access access,
                                    const std::string& rid) {
    const auto q = parse_query(req.query);
    if (!q)
        return error_response(make(400, ErrorCode::invalid_request, "bad_query",
                                   "Query parameters must be distinct name=value pairs."), rid);
    long window_s = 3600, limit = 120;
    std::string metric = "all";
    static const std::vector<std::string> kMetrics = {"cpu_busy_pct", "mem_available_bytes", "mem_total_bytes",
                                                      "memory_pressure", "psi_cpu_some10", "psi_mem_some10"};
    auto int_param = [&](const std::string& name, long lo, long hi, long& out) -> std::optional<ApiError> {
        const auto it = q->find(name);
        if (it == q->end()) return std::nullopt;
        const auto& v = it->second;
        if (v.size() > 7 || !std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return make(400, ErrorCode::invalid_request, "bad_" + name, name + " must be an integer.");
        out = std::stol(v);
        if (out < lo || out > hi)
            return make(400, ErrorCode::invalid_request, "bad_" + name,
                        name + " must be between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
        return std::nullopt;
    };
    for (const auto& [k, v] : *q)
        if (k != "window_seconds" && k != "limit" && k != "metric")
            return error_response(make(400, ErrorCode::invalid_request, "unknown_parameter",
                                       "Supported parameters: window_seconds, limit, metric."), rid);
    if (auto e = int_param("window_seconds", 1, 86'400, window_s)) return error_response(*e, rid);
    if (auto e = int_param("limit", 1, 1000, limit)) return error_response(*e, rid);
    if (auto it = q->find("metric"); it != q->end()) {
        metric = it->second;
        if (metric != "all" && std::find(kMetrics.begin(), kMetrics.end(), metric) == kMetrics.end())
            return error_response(make(400, ErrorCode::invalid_request, "unknown_metric",
                                       "metric must be all, cpu_busy_pct, mem_available_bytes, mem_total_bytes, "
                                       "memory_pressure, psi_cpu_some10 or psi_mem_some10."), rid);
    }
    (void)access;  // history points carry no executable paths or owners: nothing further to redact
    const auto hist = rt_->history_copy();
    const auto cfg = rt_->config();
    const auto now = std::chrono::steady_clock::now();
    std::vector<const HistoryPoint*> in_window;
    for (const auto& p : hist.points())
        if (now - p.at <= std::chrono::seconds(window_s)) in_window.push_back(&p);
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(limit), in_window.size());
    const auto first = in_window.end() - static_cast<std::ptrdiff_t>(n);
    auto w = envelope(rid);
    w.kv("window_seconds", static_cast<std::int64_t>(window_s)).kv("metric", metric)
        .kv("interval_ms", static_cast<std::int64_t>(cfg.interval.count()))
        .kv("capacity", static_cast<std::uint64_t>(hist.config().capacity))
        .kv("points_in_window", static_cast<std::uint64_t>(in_window.size())).kv("returned", static_cast<std::uint64_t>(n))
        .kv("truncated", n < in_window.size());
    auto want = [&](std::string_view m) { return metric == "all" || metric == m; };
    w.key("points").begin_array();
    for (auto it = first; it != in_window.end(); ++it) {
        const auto& p = **it;
        w.begin_object().kv("t", iso8601(p.wall));
        num(w, "age_s", std::chrono::duration<double>(now - p.at).count());
        if (want("cpu_busy_pct")) num(w, "cpu_busy_pct", p.cpu_busy_pct);
        if (want("mem_available_bytes")) w.key("mem_available_bytes").value(p.mem_available_bytes);
        if (want("mem_total_bytes")) w.key("mem_total_bytes").value(p.mem_total_bytes);
        if (want("memory_pressure")) {
            w.key("memory_pressure");
            if (p.pressure) w.value(to_string(*p.pressure));
            else w.null();
        }
        if (want("psi_cpu_some10")) num(w, "psi_cpu_some10", p.psi_cpu_some10);
        if (want("psi_mem_some10")) num(w, "psi_mem_some10", p.psi_mem_some10);
        w.end_object();
    }
    w.end_array();
    // Gaps are reported, never filled: consecutive points further apart than
    // two intervals plus the sampling window.
    const double gap_s = std::chrono::duration<double>(cfg.interval * 2 + cfg.sample_window).count();
    w.key("gaps").begin_array();
    for (auto it = first; it != in_window.end() && it + 1 != in_window.end(); ++it) {
        const double dt = std::chrono::duration<double>((*(it + 1))->at - (*it)->at).count();
        if (dt > gap_s) w.begin_object().kv("from", iso8601((*it)->wall)).kv("to", iso8601((*(it + 1))->wall)).kv("seconds", dt).end_object();
    }
    w.end_array();
    return ok(w);
}

// ---------------------------------------------------------------- diagnose ---

HttpResponse DiagnosticApi::diagnose(const HttpRequest& req, const PeerCredentials& peer, Access access,
                                     const std::string& header_rid, const CancellationToken& cancel) {
    std::string rid = header_rid;
    auto fail = [&](ApiError e) { return error_response(e, rid); };
    if (!req.content_type)
        return fail(make(415, ErrorCode::invalid_request, "content_type_required", "Send Content-Type: application/json."));
    if (*req.content_type != "application/json")
        return fail(make(415, ErrorCode::invalid_request, "unsupported_media_type", "Send Content-Type: application/json."));
    if (!req.content_length)
        return fail(make(411, ErrorCode::invalid_request, "length_required", "Send a Content-Length body."));
    JsonLimits lim;
    lim.max_bytes = s_.max_body_bytes;
    lim.max_members = 8;
    lim.max_string_bytes = 8192;
    auto obj = parse_flat_object(req.body, lim);
    if (!obj) return fail(make(400, ErrorCode::invalid_request, "invalid_json", obj.error().message));
    const JsonObject& o = *obj;
    for (const auto& [k, v] : o)
        if (k != "request_id" && k != "question" && k != "session_id" && k != "max_evidence_age_ms" && k != "include_evidence")
            return fail(make(400, ErrorCode::invalid_request, "unknown_field",
                             "Unknown field. Accepted: request_id, question, session_id, max_evidence_age_ms, include_evidence."));
    auto str = [&](const char* k) -> const std::string* {
        const auto it = o.find(k);
        return it == o.end() ? nullptr : std::get_if<std::string>(&it->second);
    };
    auto present = [&](const char* k) { return o.count(k) != 0 && !std::holds_alternative<std::nullptr_t>(o.at(k)); };
    if (present("request_id")) {
        const auto* v = str("request_id");
        if (v == nullptr || !id_chars(*v))
            return fail(make(400, ErrorCode::invalid_request, "bad_request_id", "request_id must be 1-64 characters of [A-Za-z0-9._:-]."));
        if (!header_rid.empty() && *v != header_rid)
            return fail(make(400, ErrorCode::invalid_request, "conflicting_request_id",
                             "request_id and X-Request-Id differ."));
        rid = *v;
    }
    const auto* question = str("question");
    if (question == nullptr)
        return fail(make(400, ErrorCode::invalid_request, "question_required", "question must be a string."));
    if (question->size() > s_.max_question_bytes)
        return fail(make(413, ErrorCode::invalid_request, "question_too_long",
                         "question is longer than " + std::to_string(s_.max_question_bytes) + " bytes."));
    if (question->find_first_not_of(" \t") == std::string::npos)
        return fail(make(400, ErrorCode::invalid_request, "question_empty", "question is empty."));
    std::string session_id;
    if (present("session_id")) {
        const auto* v = str("session_id");
        if (v == nullptr || !SessionStore::valid_id(*v))
            return fail(make(400, ErrorCode::invalid_request, "bad_session_id", "session_id must be 1-64 characters of [A-Za-z0-9._-]."));
        session_id = *v;
    }
    Millis max_age = s_.max_evidence_age;
    if (present("max_evidence_age_ms")) {
        const auto* v = std::get_if<std::int64_t>(&o.at("max_evidence_age_ms"));
        if (v == nullptr || *v < 1 || *v > 3'600'000)
            return fail(make(400, ErrorCode::invalid_request, "bad_max_evidence_age",
                             "max_evidence_age_ms must be an integer from 1 to 3600000."));
        max_age = std::min(max_age, Millis{*v});
    }
    bool include_evidence = true;
    if (present("include_evidence")) {
        const auto* v = std::get_if<bool>(&o.at("include_evidence"));
        if (v == nullptr) return fail(make(400, ErrorCode::invalid_request, "bad_include_evidence", "include_evidence must be a boolean."));
        include_evidence = *v;
    }
    if (cancel.cancelled())
        return fail(make(503, ErrorCode::busy, "stopping", "The agent is stopping.", true, 1));

    // Sessions belong to the kernel-reported uid, not to the id alone.
    if (session_id.empty()) session_id = SessionStore::generate_id();
    const auto session = sessions_.get(peer.uid, session_id);
    AgentEvidence::Options eo;
    eo.max_age = max_age;
    if (access != Access::full) eo.redact_for = peer.uid;
    eo.visibility = s_.visibility;
    AgentEvidence ev(*rt_, eo);
    const auto refused_before = RefusingOs::refused_on_this_thread();
    Answer a = engine_.ask(*question, session->context, ev, cancel);
    const bool refused_on_demand = RefusingOs::refused_on_this_thread() != refused_before;
    const auto& rep = ev.report();
    const auto& f = a.frame;
    const int retry = std::max<int>(1, static_cast<int>(rt_->config().interval.count() / 1000));

    // Outcomes that are not answers.
    if (rep.no_snapshot)
        return fail(make(503, ErrorCode::insufficient_evidence, "no_snapshot",
                         "The agent has not completed a collection yet.", true, retry));
    if (rep.stale)
        return fail(make(503, ErrorCode::stale_evidence, "stale_snapshot",
                         "The newest evidence is " + std::to_string(static_cast<long>(rep.age_ms.value_or(0))) +
                             " ms old, more than the " + std::to_string(max_age.count()) + " ms allowed.",
                         true, retry));
    const bool action = f.intent == Intent::action_request;
    if (!action && (f.intent == Intent::unsupported || f.intent == Intent::kubernetes || !f.validation_errors.empty()))
        return fail(make(422, ErrorCode::unsupported, f.intent == Intent::kubernetes ? "kubernetes_not_implemented"
                                                      : f.validation_errors.empty() ? "outside_scope"
                                                                                    : "uninterpretable",
                         first_line(a.text)));
    std::vector<ObservationKind> needed;
    for (auto k : f.observations)
        if (k != ObservationKind::system_info && k != ObservationKind::cpu_topology) needed.push_back(k);
    const bool all_missing = rep.consulted && !needed.empty() &&
                             std::all_of(needed.begin(), needed.end(), [&](ObservationKind k) {
                                 return std::find(rep.missing.begin(), rep.missing.end(), k) != rep.missing.end();
                             });
    std::vector<Limitation> lims;
    const SystemState* used = ev.used_state().get();
    for (auto k : rep.missing) {
        std::string why = "not in the agent's snapshot";
        if (used != nullptr)
            if (const auto* u = used->why_unavailable(k)) why += " (" + std::string(to_string(u->why.code)) + ")";
        lims.push_back({"observation_not_collected", std::string(to_string(k)) + ": " + why});
    }
    if (all_missing) {
        // An error that names what is missing.
        json::Writer w = envelope(rid);
        w.key("error").begin_object().kv("code", to_string(ErrorCode::insufficient_evidence))
            .kv("reason", "observations_not_collected")
            .kv("message", "The agent does not collect the observations this question needs.").kv("retryable", false)
            .end_object();
        w.key("limitations").begin_array();
        for (const auto& l : lims) w.begin_object().kv("code", l.code).kv("detail", l.detail).end_object();
        w.end_array().kv("action_executed", false);
        return ok(w, 422);
    }
    for (const auto& u : f.unavailable_observations) lims.push_back({"unsupported_on_platform", u});
    if (refused_on_demand) lims.push_back({"on_demand_observation_unavailable", std::string("part of this answer is ") + kNoOnDemand});
    if (!rep.cgroup_note.empty()) lims.push_back({"cgroup_rates", rep.cgroup_note});
    if (used != nullptr && used->processes_partial > 0)
        lims.push_back({"permission_limited", std::to_string(used->processes_partial) +
                                                  " processes of other users are not readable by the agent"});
    if (access != Access::full) lims.push_back({"redacted", "executable paths of other users' processes are withheld"});
    // A PID cannot be ambiguous: an unresolved PID target is a process that is not running.
    const bool pid_not_running = f.target.kind == intent::TargetKind::pid && f.ambiguous && !action;
    if (f.safety == intent::SafetyMode::past_event || pid_not_running)
        lims.push_back({"no_event_history", "OSIE keeps no record of past signals, exits or restarts; it reports current state"});
    if (f.intent == Intent::what_if)
        lims.push_back({"prediction_not_available", "numeric predictions are not offered through the API"});
    if (a.diagnosis)
        for (const auto& m : a.diagnosis->missing_evidence) lims.push_back({"missing_evidence", m});

    std::string status = "completed";
    if (action) status = "refused";
    else if (pid_not_running) status = "completed";
    else if (f.ambiguous && !f.clarification.empty()) status = "needs_clarification";
    else if (!rep.missing.empty() || refused_on_demand) status = "partial";

    auto w = envelope(rid);
    w.kv("session_id", session_id).kv("status", status);
    w.key("answer").begin_object();
    w.kv("summary", a.diagnosis && !a.diagnosis->headline.empty() ? a.diagnosis->headline : first_line(a.text));
    w.key("assessment");
    if (action) w.value("action_refused");
    else if (pid_not_running) w.value("process_not_running");
    else if (status == "needs_clarification") w.value("clarification_needed");
    else if (a.diagnosis && !a.diagnosis->conclusion.empty()) w.value(a.diagnosis->conclusion);
    else w.value(intent::to_string(f.intent));
    w.key("certainty");
    if (a.diagnosis) w.value(reason::to_string(a.diagnosis->certainty));
    else w.null();
    w.key("confidence").null();  // no calibrated confidence exists for diagnoses
    w.kv("text", a.text).end_object();
    w.key("interpretation").begin_object().kv("intent", intent::to_string(f.intent));
    if (f.safety != intent::SafetyMode::none) w.kv("safety", intent::to_string(f.safety));
    w.key("target").begin_object().kv("kind", intent::to_string(f.target.kind));
    if (!f.target.value.empty()) w.kv("value", f.target.value);
    w.end_object();
    if (f.temporal) {
        w.key("temporal").begin_object().kv("kind", intent::to_string(f.temporal->kind))
            .kv("metric", intent::to_string(f.temporal->metric));
        if (f.temporal->window_s) w.kv("window_s", *f.temporal->window_s);
        w.end_object();
    }
    w.end_object();
    if (rep.consulted && rep.collected_at) {
        w.key("observation").begin_object().kv("snapshot_seq", rep.seq).kv("collected_at", iso8601(*rep.collected_at));
        num(w, "age_ms", rep.age_ms);
        w.kv("max_age_ms", static_cast<std::int64_t>(max_age.count())).end_object();
    }
    if (include_evidence) {
        w.key("evidence").begin_array();
        if (a.diagnosis) {
            for (const auto& e : a.diagnosis->evidence) {
                w.begin_object().kv("signal", e.id).kv("source", e.source);
                if (!e.scope.empty()) w.kv("scope", e.scope);
                num(w, "value", e.value);
                if (!e.unit.empty()) w.kv("unit", e.unit);
                w.kv("validity", to_string(e.validity));
                if (e.interval_s) num(w, "interval_s", e.interval_s);
                if (rep.collected_at) w.kv("observed_at", iso8601(*rep.collected_at));
                if (e.permission_limited) w.kv("permission_limited", true);
                w.kv("description", e.text).end_object();
            }
        }
        w.end_array();
        w.key("processes").begin_array();
        if (used != nullptr && a.diagnosis) {
            std::vector<Pid> pids;
            if (a.diagnosis->subject_pid) pids.push_back(*a.diagnosis->subject_pid);
            for (Pid p : a.diagnosis->ranked_pids)
                if (std::find(pids.begin(), pids.end(), p) == pids.end()) pids.push_back(p);
            if (pids.size() > 20) pids.resize(20);
            for (Pid p : pids)
                if (const auto* pv = used->find_pid(p)) write_process(w, *pv);
        }
        w.end_array();
        if (!a.trends.empty()) {
            w.key("trends").begin_array();
            for (const auto& [label, t] : a.trends) serialize::write_trend(w, label, t);
            w.end_array();
        }
        w.key("diagnosis");
        if (a.diagnosis) reason::write_json(w, *a.diagnosis);
        else w.null();
    }
    w.key("limitations").begin_array();
    for (const auto& l : lims) w.begin_object().kv("code", l.code).kv("detail", l.detail).end_object();
    w.end_array();
    w.kv("action_executed", false);
    return ok(w);
}

}  // namespace osie::api
