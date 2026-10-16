// SPDX-License-Identifier: Apache-2.0
// osie-agent: the opt-in background observer (docs/AGENT.md).
//
//   osie-agent [--config PATH | --config-if-exists PATH] [--duration S] [--report]
//              [--measure-freshness] [--allow-root]
//   osie-agent --check-config PATH | --print-default-config | --once | --version
//
// Signals: SIGTERM/SIGINT stop cleanly (exit 0), SIGHUP reloads the
// configuration (an invalid file is reported and the old one kept), SIGUSR1
// writes the health status to stderr. Exit codes: 0 clean stop, 1 runtime
// error, 2 configuration or usage error (systemd does not restart on 2).
//
// The agent opens no network socket. Under systemd (Type=notify) it sends
// READY/STOPPING/STATUS to $NOTIFY_SOCKET, a local AF_UNIX datagram socket.
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "osie/agent/runtime.hpp"
#include "osie/util/json.hpp"

#ifndef OSIE_VERSION
#define OSIE_VERSION "unknown"
#endif

using namespace osie;
using namespace osie::agent;
using Clock = std::chrono::steady_clock;

namespace {

LogLevel g_level = LogLevel::info;

void log(LogLevel l, const std::string& m) {
    if (static_cast<int>(l) > static_cast<int>(g_level)) return;
    std::fprintf(stderr, "osie-agent: %s: %s\n", std::string(to_string(l)).c_str(), m.c_str());
    std::fflush(stderr);
}

/// sd_notify(3) without libsystemd. Only ever a local AF_UNIX datagram.
void notify(const std::string& msg) {
#if defined(__linux__)
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || path[0] == '\0') return;
    const std::size_t len = std::strlen(path);
    sockaddr_un sa{};
    if (len >= sizeof sa.sun_path) return;
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, path, len);
    if (sa.sun_path[0] == '@') sa.sun_path[0] = '\0';  // abstract namespace
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    (void)::sendto(fd, msg.data(), msg.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&sa),
                   static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len));
    ::close(fd);
#else
    (void)msg;
#endif
}

struct Usage {
    double cpu_s{0};
    double rss_mb{0}, peak_rss_mb{0};
};

Usage usage() {
    Usage u;
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    u.cpu_s = static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
              static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
#if defined(__linux__)
    std::ifstream st("/proc/self/status");
    for (std::string line; std::getline(st, line);) {
        if (line.rfind("VmRSS:", 0) == 0) u.rss_mb = std::strtod(line.c_str() + 6, nullptr) / 1024.0;
        if (line.rfind("VmHWM:", 0) == 0) u.peak_rss_mb = std::strtod(line.c_str() + 6, nullptr) / 1024.0;
    }
#else
    u.peak_rss_mb = static_cast<double>(r.ru_maxrss) / (1024.0 * 1024.0);
    u.rss_mb = u.peak_rss_mb;
#endif
    return u;
}

struct Options {
    std::string config_path;
    bool config_optional{false};
    std::string check_path;
    bool print_default{false}, once{false}, report{false}, measure_freshness{false}, allow_root{false};
    double duration_s{0};
};

int usage_error(const char* m) {
    std::fprintf(stderr,
                 "osie-agent: %s\n"
                 "usage: osie-agent [--config PATH | --config-if-exists PATH] [--duration S] [--report]\n"
                 "                  [--measure-freshness] [--allow-root]\n"
                 "       osie-agent --check-config PATH | --print-default-config | --once | --version\n",
                 m);
    return 2;
}

/// Signals are blocked in every thread and received by this one thread
/// (sigwait), so no handler runs asynchronously inside the runtime.
struct SignalState {
    std::mutex mu;
    std::condition_variable cv;
    bool stop{false}, reload{false}, dump{false};
    Clock::time_point stop_at{};
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--config") { if (!next(o.config_path)) return usage_error("--config needs a path"); }
        else if (a == "--config-if-exists") { if (!next(o.config_path)) return usage_error("--config-if-exists needs a path"); o.config_optional = true; }
        else if (a == "--check-config") { if (!next(o.check_path)) return usage_error("--check-config needs a path"); }
        else if (a == "--print-default-config") o.print_default = true;
        else if (a == "--once") o.once = true;
        else if (a == "--report") o.report = true;
        else if (a == "--measure-freshness") o.measure_freshness = true;
        else if (a == "--allow-root") o.allow_root = true;
        else if (a == "--duration") {
            if (!next(v)) return usage_error("--duration needs seconds");
            char* end = nullptr;
            o.duration_s = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(o.duration_s > 0) || o.duration_s > 1e7) return usage_error("bad --duration");
        } else if (a == "--version") {
            std::printf("osie-agent %s\n", OSIE_VERSION);
            return 0;
        } else {
            return usage_error(("unknown option " + a).c_str());
        }
    }
    if (o.print_default) {
        std::fputs(to_text(AgentConfig{}).c_str(), stdout);
        return 0;
    }
    if (!o.check_path.empty()) {
        auto c = load_agent_config(o.check_path);
        if (!c) {
            std::fprintf(stderr, "osie-agent: %s\n", c.error().describe().c_str());
            return 2;
        }
        std::fputs(to_text(*c).c_str(), stdout);
        return 0;
    }

#if defined(__linux__)
    // Never gain privileges through execve (setuid helpers); the agent needs none.
    (void)::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
    if (::geteuid() == 0 && !o.allow_root) {
        log(LogLevel::error, "refusing to run as root: the agent needs no privileges (use a service user, "
                             "DynamicUser=yes under systemd, or --allow-root to override)");
        return 2;
    }

    AgentConfig cfg;
    if (!o.config_path.empty()) {
        auto c = load_agent_config(o.config_path);
        if (c) cfg = *c;
        else if (o.config_optional && c.error().code == Errc::not_found) log(LogLevel::info, "no config at " + o.config_path + "; using defaults");
        else {
            log(LogLevel::error, c.error().describe());
            return 2;
        }
    }
    g_level = cfg.log_level;

    // Block the handled signals before any thread starts, so every thread inherits the mask.
    sigset_t set;
    sigemptyset(&set);
    for (int s : {SIGTERM, SIGINT, SIGHUP, SIGUSR1}) sigaddset(&set, s);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    AgentRuntime rt(make_native_os(), cfg);
    if (o.once) {
        rt.collect_now();
        auto s = rt.snapshot();
        json::Writer w;
        w.begin_object().kv("seq", s ? s->seq : 0).kv("processes",
            static_cast<std::uint64_t>(s && s->state->processes ? s->state->processes->value.size() : 0))
            .kv("cgroups", static_cast<std::uint64_t>(s ? s->state->cgroups.size() : 0)).end_object();
        std::printf("{\"snapshot\":%s,\"health\":%s,\"capabilities\":%s}\n", w.str().c_str(), to_json(rt.health()).c_str(),
                    capabilities_json(rt).c_str());
        return 0;
    }

    const auto t_start = Clock::now();
    const Usage u0 = usage();
    // Started before the signal thread, so a failure returns with no thread to join.
    // Signals that arrive meanwhile stay pending (blocked) until sigwait().
    if (auto r = rt.start(); !r) {
        log(LogLevel::error, r.error().describe());
        return 1;
    }
    SignalState sig;
    std::thread sig_thread([&] {
        while (true) {
            int s = 0;
            if (sigwait(&set, &s) != 0) continue;
            std::lock_guard lk(sig.mu);
            if (s == SIGTERM || s == SIGINT) {
                sig.stop = true;
                sig.stop_at = Clock::now();
            } else if (s == SIGHUP) {
                sig.reload = true;
            } else if (s == SIGUSR1) {
                sig.dump = true;
            }
            sig.cv.notify_all();
            if (sig.stop) return;
        }
    });

    log(LogLevel::info, "started (interval " + std::to_string(cfg.interval.count()) + " ms, window " +
                            std::to_string(cfg.sample_window.count()) + " ms, history " +
                            std::to_string(cfg.history_capacity) + " points); read-only, in memory only, no network");

    // Optional freshness probe: a reader that samples snapshot age the way an API client would.
    std::atomic<bool> probe_stop{false};
    std::vector<double> ages_ms;
    std::thread probe;
    if (o.measure_freshness) {
        probe = std::thread([&] {
            while (!probe_stop) {
                if (auto s = rt.snapshot())
                    ages_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - s->mono).count());
                std::this_thread::sleep_for(std::chrono::milliseconds(97));
                if (ages_ms.size() > 200'000) ages_ms.erase(ages_ms.begin(), ages_ms.begin() + 100'000);
            }
        });
    }

    bool ready_sent = false;
    auto last_health_log = Clock::now();
    const auto deadline = o.duration_s > 0 ? t_start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration_s))
                                           : Clock::time_point::max();
    Clock::time_point stop_at = Clock::time_point::max();
    while (true) {
        std::unique_lock lk(sig.mu);
        sig.cv.wait_until(lk, std::min(deadline, Clock::now() + std::chrono::milliseconds(200)),
                          [&] { return sig.stop || sig.reload || sig.dump; });
        if (sig.stop) {
            stop_at = sig.stop_at;
            break;
        }
        if (Clock::now() >= deadline) {
            stop_at = Clock::now();
            break;
        }
        const bool reload = std::exchange(sig.reload, false);
        const bool dump = std::exchange(sig.dump, false);
        lk.unlock();
        if (!ready_sent && rt.snapshot()) {
            notify("READY=1\nSTATUS=collecting every " + std::to_string(rt.config().interval.count()) + " ms");
            ready_sent = true;
        }
        if (reload) {
            if (o.config_path.empty()) {
                log(LogLevel::warn, "SIGHUP: no configuration file to reload");
            } else if (auto c = load_agent_config(o.config_path)) {
                rt.reconfigure(*c);
                g_level = c->log_level;
                log(LogLevel::info, "configuration reloaded from " + o.config_path);
            } else {
                log(LogLevel::error, "SIGHUP: keeping the current configuration: " + c.error().describe());
            }
        }
        if (dump) std::fprintf(stderr, "%s\n", to_json(rt.health()).c_str());
        const auto hl = rt.config().health_log_interval;
        if (hl.count() > 0 && Clock::now() - last_health_log >= hl) {
            last_health_log = Clock::now();
            const auto h = rt.health();
            log(LogLevel::info, std::string("health: ") + std::string(to_string(h.state)) + ", " +
                                    std::to_string(h.collections) + " collections, " +
                                    std::to_string(h.failed_collections) + " with failures, " +
                                    std::to_string(h.skipped_ticks) + " skipped ticks, history " +
                                    std::to_string(h.history_points) + " points");
        }
    }

    notify("STOPPING=1");
    log(LogLevel::info, "stopping");
    rt.stop();
    const double shutdown_ms = std::chrono::duration<double, std::milli>(Clock::now() - stop_at).count();
    probe_stop = true;
    if (probe.joinable()) probe.join();
    if (!sig.stop) {  // stopped by --duration: wake the signal thread
        kill(getpid(), SIGTERM);
    }
    sig_thread.join();
    log(LogLevel::info, "stopped in " + std::to_string(static_cast<long>(shutdown_ms)) + " ms");

    if (o.report) {
        const Usage u1 = usage();
        const double wall = std::chrono::duration<double>(Clock::now() - t_start).count();
        const auto h = rt.health();
        json::Writer w;
        w.begin_object().kv("version", OSIE_VERSION).kv("wall_s", wall).kv("cpu_s", u1.cpu_s - u0.cpu_s)
            .kv("cpu_share_of_one_core", (u1.cpu_s - u0.cpu_s) / std::max(1e-9, wall))
            .kv("rss_mb", u1.rss_mb).kv("peak_rss_mb", u1.peak_rss_mb).kv("shutdown_ms", shutdown_ms)
            .kv("interval_ms", static_cast<std::int64_t>(rt.config().interval.count()))
            .kv("sample_window_ms", static_cast<std::int64_t>(rt.config().sample_window.count()));
        if (!ages_ms.empty()) {
            std::sort(ages_ms.begin(), ages_ms.end());
            w.key("snapshot_age_ms").begin_object().kv("n", static_cast<std::uint64_t>(ages_ms.size()))
                .kv("p50", ages_ms[ages_ms.size() / 2]).kv("p99", ages_ms[static_cast<std::size_t>(0.99 * static_cast<double>(ages_ms.size()))])
                .kv("max", ages_ms.back()).end_object();
        }
        w.end_object();
        std::string s = w.take();
        s.pop_back();
        std::printf("%s,\"health\":%s}\n", s.c_str(), to_json(h).c_str());
    }
    return 0;
}
