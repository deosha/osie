// SPDX-License-Identifier: Apache-2.0
#include "osie/agent/runtime.hpp"

#include <algorithm>
#include <cmath>

#include "osie/util/json.hpp"

namespace osie::agent {
namespace {

double ms_between(SteadyTime a, SteadyTime b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::int64_t unix_ms(WallTime t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

}  // namespace

std::string_view to_string(AgentState s) noexcept {
    switch (s) {
        case AgentState::stopped: return "stopped";
        case AgentState::starting: return "starting";
        case AgentState::running: return "running";
        case AgentState::degraded: return "degraded";
        case AgentState::stopping: return "stopping";
    }
    return "?";
}

AgentRuntime::AgentRuntime(std::unique_ptr<IOperatingSystem> os, AgentConfig cfg)
    : os_(std::move(os)),
      reg_(CapabilityRegistry::builtin()),
      audit_(256),
      broker_(*os_, reg_, policy_, audit_),
      sampler_(broker_),
      cfg_(std::move(cfg)),
      history_(HistoryConfig{cfg_.history_capacity, cfg_.history_top_k, 10.0}) {
    reconfigure(cfg_);
}

// No other thread may be inside a member call (the lifetime contract), so
// stop() here only has the scheduler thread to cancel and join.
AgentRuntime::~AgentRuntime() { stop(); }

void AgentRuntime::reconfigure(const AgentConfig& cfg) {
    // Held throughout, so concurrent reconfigurations apply one after the
    // other and the configuration always matches the history bounds.
    std::lock_guard cl(cfg_mu_);
    cfg_ = cfg;
    const auto sup = os_->supported();
    std::lock_guard hl(hist_mu_);
    if (history_.config().capacity != cfg.history_capacity || history_.config().top_k != cfg.history_top_k) {
        history_.reconfigure(HistoryConfig{cfg.history_capacity, cfg.history_top_k, history_.config().min_span_s});
        health_.history_points = history_.size();
        health_.history_bytes = history_.approx_bytes();
    }
    // Per-kind health for the observed kinds, keeping counters of kinds still observed.
    std::vector<KindHealth> kinds;
    for (auto k : cfg.observe) {
        KindHealth kh;
        for (const auto& old : health_.kinds)
            if (old.kind == k) kh = old;
        kh.kind = k;
        kh.supported = sup.contains(k);
        const auto* cap = reg_.for_observation(k);
        kh.allowed = cap != nullptr && policy_.authorize(*cap, os_->platform(), sup).allowed;
        kinds.push_back(std::move(kh));
    }
    health_.kinds = std::move(kinds);
}

AgentConfig AgentRuntime::config() const {
    std::lock_guard lk(cfg_mu_);
    return cfg_;
}

Result<void> AgentRuntime::start() {
    std::lock_guard rl(run_mu_);
    if (thread_.joinable()) return Error{Errc::invalid_argument, "agent already running", 0};
    {
        std::lock_guard lk(life_mu_);
        stop_requested_ = false;
    }
    {
        std::lock_guard hl(hist_mu_);
        health_.started_at = std::chrono::system_clock::now();
    }
    state_ = AgentState::starting;
    thread_ = std::thread([this] { loop(); });
    return {};
}

void AgentRuntime::stop() {
    std::lock_guard rl(run_mu_);
    const bool scheduler = thread_.joinable();
    if (scheduler) state_ = AgentState::stopping;
    {
        // Close admission, then cancel whatever is in flight (scheduled or manual).
        std::lock_guard lk(life_mu_);
        closed_ = true;
        stop_requested_ = true;
        active_.cancel();
        wake_.notify_all();
    }
    if (scheduler) thread_.join();
    {
        std::unique_lock lk(life_mu_);
        idle_.wait(lk, [this] { return !in_flight_; });
        closed_ = false;
    }
    state_ = AgentState::stopped;
}

bool AgentRuntime::running() const noexcept {
    const auto s = state_.load();
    return s == AgentState::starting || s == AgentState::running || s == AgentState::degraded;
}

void AgentRuntime::set_live_state(AgentState to) {
    auto cur = state_.load();
    while ((cur == AgentState::starting || cur == AgentState::running || cur == AgentState::degraded) &&
           !state_.compare_exchange_weak(cur, to)) {
    }
}

AgentRuntime::Admit AgentRuntime::begin_collection(CancellationToken& token) {
    std::lock_guard lk(life_mu_);
    if (closed_) return Admit::closed;
    if (in_flight_) return Admit::busy;
    in_flight_ = true;
    active_ = CancellationSource{};
    token = active_.token();
    return Admit::ok;
}

void AgentRuntime::end_collection() {
    // Notified under the lock: stop() (and so the destructor) cannot proceed
    // until this thread has released life_mu_, after which it touches nothing.
    std::lock_guard lk(life_mu_);
    in_flight_ = false;
    idle_.notify_all();
}

void AgentRuntime::record_jitter(double ms) {
    jitter_[jitter_n_ % jitter_.size()] = ms;
    ++jitter_n_;
}

void AgentRuntime::loop() {
    auto next = std::chrono::steady_clock::now();
    while (true) {
        {
            std::unique_lock lk(life_mu_);
            if (wake_.wait_until(lk, next, [this] { return stop_requested_; })) break;
        }
        const auto started = std::chrono::steady_clock::now();
        {
            std::lock_guard hl(hist_mu_);
            record_jitter(ms_between(next, started));
        }
        CancellationToken token;
        switch (begin_collection(token)) {
            case Admit::ok:
                (void)collect(token);
                end_collection();
                break;
            case Admit::busy: {  // a manual collection holds the slot: this tick is skipped
                std::lock_guard hl(hist_mu_);
                ++health_.skipped_ticks;
                break;
            }
            case Admit::closed: return;
        }
        // Fixed cadence on the monotonic clock. A collection that overran one
        // or more ticks skips them (counted) instead of running back to back.
        const auto interval = config().interval;
        next += interval;
        const auto now = std::chrono::steady_clock::now();
        if (next <= now) {
            const auto missed = static_cast<std::uint64_t>((now - next) / interval) + 1;
            next += interval * static_cast<long>(missed);
            std::lock_guard hl(hist_mu_);
            health_.skipped_ticks += missed;
        }
    }
}

bool AgentRuntime::collect_now() {
    CancellationToken token;
    if (begin_collection(token) != Admit::ok) {
        std::lock_guard hl(hist_mu_);
        ++health_.rejected_manual;
        return false;
    }
    const bool published = collect(token);
    end_collection();
    return published;
}

bool AgentRuntime::collect(const CancellationToken& cancel) {
    const AgentConfig cfg = config();
    const auto t0 = std::chrono::steady_clock::now();
    SampleRequest req;
    req.interval = cfg.sample_window;
    req.cgroup_top_n = cfg.cgroup_top_n;
    const auto sup = os_->supported();
    for (auto k : cfg.observe)
        if (sup.contains(k)) req.kinds.push_back(k);
    for (auto k : {ObservationKind::system_info, ObservationKind::cpu_topology})
        if (sup.contains(k) && std::find(req.kinds.begin(), req.kinds.end(), k) == req.kinds.end()) req.kinds.push_back(k);
    auto st = std::make_shared<SystemState>(sampler_.sample(req, cancel));
    // A collection interrupted by shutdown is incomplete: it is not published.
    if (cancel.cancelled()) return false;
    const auto t1 = std::chrono::steady_clock::now();
    const auto wall = std::chrono::system_clock::now();

    bool failed_any = false;
    std::vector<std::string> degraded;
    {
        std::lock_guard hl(hist_mu_);
        for (auto& kh : health_.kinds) {
            if (!kh.supported || !kh.allowed) continue;
            const auto* why = st->why_unavailable(kh.kind);
            bool ok = why == nullptr && st->has(kh.kind);
            if (kh.kind == ObservationKind::cgroup) {
                // Cgroups are read for the busiest processes; one that exited
                // (or has no cgroup) in between is not a collector failure.
                // It fails only when nothing could be read for another reason.
                ok = !st->cgroups.empty() || why == nullptr || why->why.code == Errc::process_exited ||
                     why->why.code == Errc::not_found;
            }
            if (ok) {
                ++kh.ok;
                kh.consecutive_failures = 0;
                kh.last_ok = wall;
            } else {
                failed_any = true;
                ++kh.failed;
                ++kh.consecutive_failures;
                kh.last_error = why ? why->why.describe() : "no data returned";
                if (kh.consecutive_failures >= cfg.failure_threshold)
                    degraded.push_back(std::string(to_string(kh.kind)) + ": " + kh.last_error + " (" +
                                       std::to_string(kh.consecutive_failures) + " consecutive collections)");
            }
        }
        history_.record(*st);
        // Computed once per collection, so readers never walk the history under the lock.
        health_.history_points = history_.size();
        health_.history_bytes = history_.approx_bytes();
        ++health_.collections;
        if (failed_any) ++health_.failed_collections;
        health_.last_collection_wall = wall;
        health_.last_collection_ms = ms_between(t0, t1);
        health_.degraded_reasons = degraded;
    }
    auto snap = std::make_shared<AgentSnapshot>();
    snap->wall = wall;
    snap->mono = st->taken_at;
    snap->state = std::move(st);
    {
        std::lock_guard sl(snap_mu_);
        snap->seq = ++seq_;
        prev_snap_ = std::move(snap_);
        snap_ = std::move(snap);
    }
    set_live_state(degraded.empty() ? AgentState::running : AgentState::degraded);
    return true;
}

std::shared_ptr<const AgentSnapshot> AgentRuntime::snapshot() const {
    std::lock_guard sl(snap_mu_);
    return snap_;
}

std::pair<std::shared_ptr<const AgentSnapshot>, std::shared_ptr<const AgentSnapshot>> AgentRuntime::snapshots() const {
    std::lock_guard sl(snap_mu_);
    return {snap_, prev_snap_};
}

StateHistory AgentRuntime::history_copy() const {
    std::lock_guard hl(hist_mu_);
    return StateHistory(history_.config(), history_.points());
}

std::vector<HistoryPoint> AgentRuntime::history(std::size_t max) const {
    std::lock_guard hl(hist_mu_);
    const auto& pts = history_.points();
    const std::size_t n = std::min(max, pts.size());
    return {pts.end() - static_cast<std::ptrdiff_t>(n), pts.end()};
}

HealthStatus AgentRuntime::health() const {
    HealthStatus h;
    std::vector<double> j;
    std::size_t jn = 0;
    {
        // Copy under the lock; compute outside it.
        std::lock_guard hl(hist_mu_);
        h = health_;
        jn = jitter_n_;
        j.assign(jitter_.begin(), jitter_.begin() + static_cast<std::ptrdiff_t>(std::min(jitter_n_, jitter_.size())));
    }
    if (!j.empty()) {
        std::sort(j.begin(), j.end());
        h.start_jitter.n = jn;
        h.start_jitter.p50_ms = j[j.size() / 2];
        h.start_jitter.p99_ms = j[std::min(j.size() - 1, static_cast<std::size_t>(0.99 * static_cast<double>(j.size())))];
        h.start_jitter.max_ms = j.back();
    }
    h.state = state_.load();
    if (auto s = snapshot())
        h.last_collection_age_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->mono).count();
    return h;
}

std::string to_json(const HealthStatus& h) {
    json::Writer w;
    w.begin_object().kv("state", to_string(h.state));
    if (h.started_at) w.kv("started_at_unix_ms", unix_ms(*h.started_at));
    w.kv("collections", h.collections).kv("failed_collections", h.failed_collections)
        .kv("skipped_ticks", h.skipped_ticks).kv("rejected_manual_collections", h.rejected_manual);
    if (h.last_collection_wall) w.kv("last_collection_unix_ms", unix_ms(*h.last_collection_wall));
    if (h.last_collection_age_s) w.kv("last_collection_age_s", *h.last_collection_age_s);
    w.kv("last_collection_ms", h.last_collection_ms);
    w.key("start_jitter").begin_object().kv("n", static_cast<std::uint64_t>(h.start_jitter.n))
        .kv("p50_ms", h.start_jitter.p50_ms).kv("p99_ms", h.start_jitter.p99_ms).kv("max_ms", h.start_jitter.max_ms)
        .end_object();
    w.kv("history_points", static_cast<std::uint64_t>(h.history_points))
        .kv("history_bytes", static_cast<std::uint64_t>(h.history_bytes));
    w.key("observations").begin_array();
    for (const auto& k : h.kinds) {
        w.begin_object().kv("kind", to_string(k.kind)).kv("supported", k.supported).kv("allowed", k.allowed)
            .kv("ok", k.ok).kv("failed", k.failed).kv("consecutive_failures", static_cast<std::uint64_t>(k.consecutive_failures));
        if (!k.last_error.empty()) w.kv("last_error", k.last_error);
        if (k.last_ok) w.kv("last_ok_unix_ms", unix_ms(*k.last_ok));
        w.end_object();
    }
    w.end_array().key("degraded_reasons").begin_array();
    for (const auto& r : h.degraded_reasons) w.value(r);
    w.end_array().end_object();
    return w.take();
}

std::string capabilities_json(const AgentRuntime& r) {
    json::Writer w;
    write_capabilities(w, r);
    return w.take();
}

void write_capabilities(json::Writer& w, const AgentRuntime& r) {
    SafetyPolicy pol;
    w.begin_array();
    for (const auto& c : r.registry().all()) {
        const auto d = pol.authorize(c, r.platform(), r.supported());
        w.begin_object().kv("id", c.id).kv("kind", c.kind == CapabilityKind::observe ? "observe" : "act")
            .kv("platform_supported", c.platforms.contains(r.platform())).kv("allowed", d.allowed)
            .kv("reason", d.reason).end_object();
    }
    w.end_array();
}

}  // namespace osie::agent
