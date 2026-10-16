// SPDX-License-Identifier: Apache-2.0
// Load generator for the local API (docs/BENCHMARKS.md, "Local API").
//
//   osie_api_bench <socket> [--requests N] [--clients C] [--seconds S] [--burst B]
//
// Phases, each printed as one JSON line:
//   1. sequential latency per endpoint (N requests each): p50, p99, max;
//   2. C concurrent clients for S seconds, each a loop of common diagnoses:
//      throughput and latency of answered requests, and refusals by status;
//   3. admission pressure: B connections opened at once, each sending one
//      diagnosis: how many were answered and how many refused, by status.
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

struct Result {
    int status{0};
    double ms{0};
};

Result call(const std::string& sock, const std::string& req) {
    const auto t0 = Clock::now();
    Result r;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, sock.c_str(), std::min(sock.size(), sizeof sa.sun_path - 1));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        ::close(fd);
        r.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return r;
    }
    std::size_t off = 0;
    while (off < req.size()) {
        const auto n = ::write(fd, req.data() + off, req.size() - off);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
    }
    std::string resp;
    char buf[8192];
    while (true) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 10'000) <= 0) break;
        const auto n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (resp.rfind("HTTP/1.1 ", 0) == 0 && resp.size() > 12) r.status = std::atoi(resp.c_str() + 9);
    r.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return r;
}

std::string get(const std::string& target) { return "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n"; }
std::string diagnose(const std::string& q) {
    const std::string body = "{\"question\":\"" + q + "\",\"include_evidence\":true}";
    return "POST /v1/diagnose HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))];
}

const char* kQuestions[] = {"what is using the most CPU?", "why is my machine slow?", "which process uses the most memory?",
                            "is memory pressure increasing?", "why is it using so much CPU?", "is a container being throttled?"};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: osie_api_bench <socket> [--requests N] [--clients C] [--seconds S] [--burst B]\n");
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);  // the server may close a refused connection while we still write
    const std::string sock = argv[1];
    int requests = 200, clients = 16, burst = 200;
    double seconds = 10;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--requests") requests = std::atoi(argv[i + 1]);
        else if (a == "--clients") clients = std::atoi(argv[i + 1]);
        else if (a == "--seconds") seconds = std::atof(argv[i + 1]);
        else if (a == "--burst") burst = std::atoi(argv[i + 1]);
    }

    // 1. Sequential latency.
    const std::vector<std::pair<std::string, std::string>> phases = {
        {"health_live", get("/v1/health/live")},
        {"health_ready", get("/v1/health/ready")},
        {"snapshot", get("/v1/system/snapshot")},
        {"history_120", get("/v1/system/history?limit=120")},
        {"history_full", get("/v1/system/history?limit=1000&window_seconds=86400")},
        {"diagnose_top_cpu", diagnose(kQuestions[0])},
        {"diagnose_slow", diagnose(kQuestions[1])},
        {"diagnose_top_memory", diagnose(kQuestions[2])},
        {"diagnose_trend", diagnose(kQuestions[3])},
    };
    for (const auto& [name, req] : phases) {
        std::vector<double> ms;
        std::map<int, int> statuses;
        for (int i = 0; i < requests; ++i) {
            const auto r = call(sock, req);
            ++statuses[r.status];
            if (r.status == 200) ms.push_back(r.ms);
        }
        std::printf("{\"phase\":\"sequential\",\"endpoint\":\"%s\",\"n\":%zu,\"p50_ms\":%.3f,\"p99_ms\":%.3f,\"max_ms\":%.3f,"
                    "\"non_200\":%d}\n",
                    name.c_str(), ms.size(), pct(ms, 0.5), pct(ms, 0.99), ms.empty() ? 0 : *std::max_element(ms.begin(), ms.end()),
                    requests - static_cast<int>(ms.size()));
    }

    // 2. Concurrent clients.
    {
        std::atomic<bool> stop{false};
        std::mutex mu;
        std::vector<double> ms;
        std::map<int, int> statuses;
        std::vector<std::thread> ts;
        const auto t0 = Clock::now();
        for (int c = 0; c < clients; ++c)
            ts.emplace_back([&, c] {
                std::vector<double> mine;
                std::map<int, int> st;
                for (int i = 0; !stop; ++i) {
                    const auto r = call(sock, diagnose(kQuestions[(c + i) % 6]));
                    ++st[r.status];
                    if (r.status == 200) mine.push_back(r.ms);
                }
                std::lock_guard lk(mu);
                ms.insert(ms.end(), mine.begin(), mine.end());
                for (auto [k, v] : st) statuses[k] += v;
            });
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        stop = true;
        for (auto& t : ts) t.join();
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("{\"phase\":\"concurrent\",\"clients\":%d,\"seconds\":%.1f,\"answered\":%zu,\"per_s\":%.1f,\"p50_ms\":%.3f,"
                    "\"p99_ms\":%.3f,\"statuses\":{",
                    clients, wall, ms.size(), static_cast<double>(ms.size()) / wall, pct(ms, 0.5), pct(ms, 0.99));
        bool first = true;
        for (auto [k, v] : statuses) {
            std::printf("%s\"%d\":%d", first ? "" : ",", k, v);
            first = false;
        }
        std::printf("}}\n");
    }

    // 3. Admission pressure: a burst of simultaneous connections.
    {
        std::mutex mu;
        std::map<int, int> statuses;
        std::vector<double> ms;
        std::vector<std::thread> ts;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        for (int i = 0; i < burst; ++i)
            ts.emplace_back([&, i] {
                ++ready;
                while (!go) std::this_thread::yield();
                const auto r = call(sock, diagnose(kQuestions[i % 6]));
                std::lock_guard lk(mu);
                ++statuses[r.status];
                if (r.status == 200) ms.push_back(r.ms);
            });
        while (ready < burst) std::this_thread::yield();
        go = true;
        for (auto& t : ts) t.join();
        std::printf("{\"phase\":\"burst\",\"connections\":%d,\"answered\":%zu,\"p99_answered_ms\":%.3f,\"statuses\":{", burst,
                    ms.size(), pct(ms, 0.99));
        bool first = true;
        for (auto [k, v] : statuses) {
            std::printf("%s\"%d\":%d", first ? "" : ",", k, v);
            first = false;
        }
        std::printf("}}\n");
    }
    return 0;
}
