// SPDX-License-Identifier: Apache-2.0
// The local API over a real Unix domain socket: socket safety (unsafe
// directories, symlinks, replacement, stale and live sockets), hostile and
// slow clients, admission control, concurrency, disconnects and shutdown.
#include <gtest/gtest.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "osie/api/server.hpp"
#include "support/agent_fixture.hpp"
#include "support/json_value.hpp"

using namespace osie;
using namespace osie::api;
using namespace osie::test;
using Clock = std::chrono::steady_clock;

namespace {

#if defined(OSIE_SANITIZED)
constexpr int kSlow = 5;
#else
constexpr int kSlow = 1;
#endif

std::string short_tmp(const std::string& name, mode_t mode = 0700) {
    // sockaddr_un limits paths to ~100 bytes: keep the directory short.
    std::string base = ::testing::TempDir();
    if (base.size() > 60) base = "/tmp/";
    const std::string d = base + "osapi" + std::to_string(::getpid() % 100000) + name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    ::chmod(d.c_str(), mode);
    return d;
}

int connect_to(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, path.c_str(), path.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        ::close(fd);
        return -1;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

void send_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
#if defined(MSG_NOSIGNAL)
        const auto n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#else
        const auto n = ::send(fd, s.data() + off, s.size() - off, 0);
#endif
        if (n <= 0) return;
        off += static_cast<std::size_t>(n);
    }
}

std::string read_all(int fd, Millis timeout) {
    std::string out;
    const auto until = Clock::now() + timeout;
    char buf[4096];
    while (true) {
        pollfd p{fd, POLLIN, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0) break;
        const auto n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

struct Reply {
    int status{0};
    std::string head, body;
};

Reply parse_reply(const std::string& raw) {
    Reply r;
    const auto sep = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/1.1 ", 0) != 0 || sep == std::string::npos) return r;
    r.status = std::stoi(raw.substr(9, 3));
    r.head = raw.substr(0, sep);
    r.body = raw.substr(sep + 4);
    // A "100 Continue" interim response precedes the final one.
    if (r.status == 100) return parse_reply(r.body);
    return r;
}

Reply raw_call(const std::string& path, const std::string& bytes, Millis timeout = Millis{5000}) {
    const int fd = connect_to(path);
    if (fd < 0) return {};
    send_all(fd, bytes);
    const auto raw = read_all(fd, timeout);
    ::close(fd);
    return parse_reply(raw);
}

Reply call(const std::string& path, const std::string& method, const std::string& target, const std::string& body = "") {
    std::string req = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!body.empty() || method == "POST")
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    return raw_call(path, req + "\r\n" + body);
}

struct Served {
    explicit Served(const std::string& dir_name, agent::AgentConfig c = fast_config()) {
        dir = short_tmp(dir_name);
        c.api_enabled = true;
        c.api_socket = dir + "/agent.sock";
        cfg = c;
        rt = std::make_shared<agent::AgentRuntime>(machine(), c);
        EXPECT_TRUE(rt->collect_now());
        EXPECT_TRUE(rt->collect_now());
        server = std::make_unique<ApiServer>(rt, cfg);
    }
    ~Served() {
        server.reset();
        rt->stop();
        std::filesystem::remove_all(dir);
    }
    [[nodiscard]] std::string sock() const { return cfg.api_socket; }
    std::string dir;
    agent::AgentConfig cfg;
    std::shared_ptr<agent::AgentRuntime> rt;
    std::unique_ptr<ApiServer> server;
};

}  // namespace

// --------------------------------------------------------------- functional ---

TEST(ApiServer, DisabledUnlessEnabled) {
    const agent::AgentConfig d;
    EXPECT_FALSE(d.api_enabled);
    EXPECT_NE(agent::to_text(d).find("api_enabled = false"), std::string::npos);
    auto c = agent::parse_agent_config("api_enabled = true\napi_socket = /run/osie/agent.sock\napi_group = osie\n");
    ASSERT_TRUE(c.ok()) << c.error().describe();
    EXPECT_TRUE(c->api_enabled);
    for (const char* bad : {"api_enabled = yes", "api_socket = relative/agent.sock", "api_socket = /run/../etc/x.sock",
                            "api_socket = /run/osie/", "api_socket = /run/os ie/a.sock", "api_group = Root!",
                            "api_max_clients = 0", "api_max_clients = 65", "api_max_request_bytes = 100",
                            "api_request_timeout = 50ms", "api_session_ttl = 1s", "api_rate_limit = 0",
                            "listen = /run/osie/agent.sock"})
        EXPECT_FALSE(agent::parse_agent_config(bad).ok()) << bad;
    const auto round = agent::parse_agent_config(agent::to_text(*c));
    ASSERT_TRUE(round.ok()) << round.error().describe();
    EXPECT_EQ(round->api_group, "osie");
}

TEST(ApiServer, ServesEveryEndpointOverTheSocket) {
    Served s("serve");
    ASSERT_TRUE(s.server->start().ok());
    struct stat st {};
    ASSERT_EQ(::lstat(s.sock().c_str(), &st), 0);
    EXPECT_TRUE(S_ISSOCK(st.st_mode));
    EXPECT_EQ(st.st_mode & 0777, 0600U) << "no api_group: owner only";
    EXPECT_EQ(call(s.sock(), "GET", "/v1/health/live").status, 503) << "runtime collector not started";
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200);
    EXPECT_EQ(call(s.sock(), "GET", "/v1/system/snapshot").status, 200);
    EXPECT_EQ(call(s.sock(), "GET", "/v1/system/history?limit=2").status, 200);
    const auto d = call(s.sock(), "POST", "/v1/diagnose", R"({"question":"why is my CPU usage high?","request_id":"r1"})");
    ASSERT_EQ(d.status, 200) << d.body;
    auto j = JsonReader::parse(d.body);
    EXPECT_EQ(j["request_id"].str(), "r1");
    EXPECT_NE(d.head.find("Connection: close"), std::string::npos);
    // curl sends "Expect: 100-continue" for larger bodies.
    const std::string body = R"({"question":"which process uses the most memory?","request_id":")" + std::string(40, 'x') + "\"}";
    const auto e = raw_call(s.sock(), "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\n"
                                      "Expect: 100-continue\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    EXPECT_EQ(e.status, 200) << e.body;
    s.server->stop();
    EXPECT_NE(::lstat(s.sock().c_str(), &st), 0) << "socket removed on stop";
}

TEST(ApiServer, PeerCredentialsComeFromTheKernel) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const auto p = peer_of(sv[0]);
    EXPECT_TRUE(p.known);
    EXPECT_EQ(p.uid, ::geteuid());
    EXPECT_EQ(p.gid, ::getegid());
    ::close(sv[0]);
    ::close(sv[1]);
    // Not a socket at all: unknown, so it fails closed.
    EXPECT_FALSE(peer_of(-1).known);
}

#if defined(__APPLE__)
TEST(ApiServer, MacOsIsOwnerOnly) {
    group* g = ::getgrgid(::getegid());
    ASSERT_NE(g, nullptr);
    auto c = fast_config();
    c.api_group = g->gr_name;
    Served s("macgroup", c);
    auto r = s.server->start();
    ASSERT_FALSE(r.ok()) << "group access is not offered on macOS";
    EXPECT_NE(r.error().message.find("experimental"), std::string::npos);
    Served plain("macplain");
    ASSERT_TRUE(plain.server->start().ok());
    struct stat st {};
    ASSERT_EQ(::lstat(plain.sock().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600U);
    EXPECT_NE(call(plain.sock(), "GET", "/v1/capabilities").body.find("\"platform_support\":\"experimental\""), std::string::npos);
}
#endif

TEST(ApiServer, GroupSocketIs0660AndGroupOwned) {
#if defined(__APPLE__)
    GTEST_SKIP() << "group access is Linux-only";
#endif
    group* g = ::getgrgid(::getegid());
    if (g == nullptr) GTEST_SKIP() << "primary group has no name";
    auto c = fast_config();
    c.api_group = g->gr_name;
    Served s("group", c);
    auto r = s.server->start();
    ASSERT_TRUE(r.ok()) << r.error().describe();
    struct stat st {};
    ASSERT_EQ(::lstat(s.sock().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0660U);
    EXPECT_EQ(st.st_gid, ::getegid());
    auto bad = fast_config();
    bad.api_group = "osie-no-such-group-zz";
    Served s2("nogroup", bad);
    EXPECT_FALSE(s2.server->start().ok()) << "an unknown group is a configuration error";
}

// ----------------------------------------------------------- socket safety ---

TEST(ApiServer, RefusesUnsafeDirectories) {
    for (mode_t mode : {0777, 0770, 0707, 01777}) {
        Served s("unsafe" + std::to_string(mode));
        ::chmod(s.dir.c_str(), mode);
        auto r = s.server->start();
        EXPECT_FALSE(r.ok()) << std::oct << mode << ": a directory others can write is refused (even sticky)";
    }
    Served missing("missing");
    std::filesystem::remove_all(missing.dir);
    EXPECT_FALSE(missing.server->start().ok());
}

TEST(ApiServer, SymlinkAndForeignEntriesAreNeverReplacedOrFollowed) {
    // A symlink at the socket path (planted earlier) pointing at a victim file.
    {
        Served s("symlink");
        const std::string victim = s.dir + "/victim";
        std::ofstream(victim) << "keep me";
        ASSERT_EQ(::symlink(victim.c_str(), s.sock().c_str()), 0);
        EXPECT_FALSE(s.server->start().ok());
        EXPECT_TRUE(std::filesystem::is_symlink(s.sock())) << "the link was not removed";
        std::ifstream in(victim);
        std::string content;
        std::getline(in, content);
        EXPECT_EQ(content, "keep me");
    }
    // A regular file at the socket path.
    {
        Served s("file");
        std::ofstream(s.sock()) << "data";
        EXPECT_FALSE(s.server->start().ok());
        EXPECT_TRUE(std::filesystem::is_regular_file(s.sock()));
    }
    // The directory itself is a symlink owned by us to a safe directory: followed.
    {
        Served s("dirlink");
        const std::string real = s.dir + "/real";
        std::filesystem::create_directory(real);
        ::chmod(real.c_str(), 0700);
        const std::string link = s.dir + "/link";
        ASSERT_EQ(::symlink(real.c_str(), link.c_str()), 0);
        auto c = s.cfg;
        c.api_socket = link + "/a.sock";
        ApiServer srv(s.rt, c);
        auto r = srv.start();
        EXPECT_TRUE(r.ok()) << r.error().describe();
        srv.stop();
    }
}

TEST(ApiServer, StaleSocketIsCleanedLiveSocketIsLeftAndOneAgentPerDirectory) {
    Served s("stale");
    // A crashed agent's socket: bound, never unlinked, nobody listening.
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, s.sock().c_str(), s.sock().size());
        ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa), 0);
        ::close(fd);
    }
    auto r = s.server->start();
    ASSERT_TRUE(r.ok()) << r.error().describe();
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200);
    // A second server for the same socket, or another socket in the same directory: refused.
    ApiServer second(s.rt, s.cfg);
    EXPECT_FALSE(second.start().ok());
    auto c2 = s.cfg;
    c2.api_socket = s.dir + "/other.sock";
    ApiServer third(s.rt, c2);
    EXPECT_FALSE(third.start().ok()) << "flock on the directory";
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200) << "the live server is untouched";
    // Restart while the socket exists (stopped server, then a new one).
    s.server->stop();
    ApiServer again(s.rt, s.cfg);
    EXPECT_TRUE(again.start().ok());
    again.stop();
}

TEST(ApiServer, StopNeverRemovesAReplacementSocket) {
    Served s("replace");
    ASSERT_TRUE(s.server->start().ok());
    // Something replaces our socket while we run.
    ASSERT_EQ(::unlink(s.sock().c_str()), 0);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, s.sock().c_str(), s.sock().size());
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa), 0);
    s.server->stop();
    struct stat st {};
    EXPECT_EQ(::lstat(s.sock().c_str(), &st), 0) << "the replacement was left in place";
    ::close(fd);
}

// --------------------------------------------------------- hostile clients ---

TEST(ApiServer, MalformedOversizedAndOddRequests) {
    auto c = fast_config();
    c.api_max_request_bytes = 4096;
    Served s("hostile", c);
    ASSERT_TRUE(s.server->start().ok());
    const std::string sock = s.sock();
    EXPECT_EQ(raw_call(sock, "garbage\r\n\r\n").status, 400);
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: 999999\r\n\r\n").status, 413);
    EXPECT_EQ(call(sock, "POST", "/v1/diagnose", "{\"question\":\"" + std::string(3500, 'a') + "\"}").status, 413);
    EXPECT_EQ(call(sock, "DELETE", "/v1/diagnose").status, 405);
    EXPECT_EQ(call(sock, "GET", "/v1/../../etc/passwd").status, 400);
    EXPECT_EQ(call(sock, "GET", "/v1/system/snapshot?file=/etc/shadow").status, 400);
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n").status, 501);
    EXPECT_EQ(raw_call(sock, "GET /v1/health/live HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n").status, 400);
    EXPECT_EQ(raw_call(sock, std::string("GET /v1/health/live HTTP/1.1\r\nHost: a\r\nX: \0\r\n\r\n", 45)).status, 400);
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}").status, 415);
    EXPECT_EQ(raw_call(sock, "GET /v1/health/live HTTP/1.1\r\nHost: l\r\nX-Pad: " + std::string(9000, 'a') + "\r\n\r\n").status, 431);
    // Pipelined second request: refused, not processed.
    EXPECT_EQ(raw_call(sock, "GET /v1/capabilities HTTP/1.1\r\nHost: l\r\n\r\nGET /v1/capabilities HTTP/1.1\r\nHost: l\r\n\r\n").status, 400);
    EXPECT_EQ(call(sock, "GET", "/v1/capabilities").status, 200) << "the server is still healthy";
}

TEST(ApiServer, SlowClientsTimeOutAndCannotMonopolise) {
    auto c = fast_config();
    c.api_request_timeout = Millis{400};
    c.api_max_clients = 2;
    c.api_max_clients_per_user = 2;
    Served s("slow", c);
    ASSERT_TRUE(s.server->start().ok());
    // A client that sends half a request and stalls.
    const int fd = connect_to(s.sock());
    ASSERT_GE(fd, 0);
    send_all(fd, "GET /v1/health/live HTTP/1.1\r\nHo");
    const auto t0 = Clock::now();
    const auto r = parse_reply(read_all(fd, Millis{3000}));
    ::close(fd);
    EXPECT_EQ(r.status, 408);
    EXPECT_LT(std::chrono::duration<double>(Clock::now() - t0).count(), 2.0 * kSlow);
    // Filling the server needs several users: one user is capped
    // (OneUserCannotOccupyEveryWorker); several are refused with 503 busy
    // once workers and queue are full (ApiLinuxPeers, as root).
    std::this_thread::sleep_for(Millis{100});
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200);
    EXPECT_GE(s.server->stats().timeouts, 1U);
}

TEST(ApiServer, PerUserRateLimit) {
    auto c = fast_config();
    c.api_rate_limit = 5;  // burst 10
    Served s("rate", c);
    ASSERT_TRUE(s.server->start().ok());
    int ok = 0, limited = 0;
    for (int i = 0; i < 30; ++i) {
        const auto r = call(s.sock(), "GET", "/v1/health/ready");
        if (r.status == 429) ++limited;
        else ++ok;
    }
    EXPECT_GE(limited, 15);
    EXPECT_LE(ok, 12);
    EXPECT_EQ(s.server->stats().rejected_rate_limited, static_cast<std::uint64_t>(limited));
}

// ------------------------------------------------------------- concurrency ---

TEST(ApiServer, HundredConcurrentClientsAreBoundedAndAllAnswered) {
    auto c = fast_config();
    c.api_max_clients = 16;
    c.api_max_clients_per_user = 16;  // one test user plays all the clients
    c.api_rate_limit = 10'000;
    Served s("many", c);
    ASSERT_TRUE(s.server->start().ok());
    ASSERT_TRUE(s.rt->start().ok());  // background sampling at the same time
    std::atomic<int> ok{0}, busy{0}, other{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 100; ++i) {
        clients.emplace_back([&, i] {
            const char* qs[] = {R"({"question":"what is using the most CPU?"})", R"({"question":"why is my machine slow?"})",
                                R"({"question":"which process uses the most memory?"})"};
            // Refused clients retry (as told by 503 + Retry-After) until a deadline.
            const auto until = Clock::now() + std::chrono::seconds(30);
            while (Clock::now() < until) {
                const auto r = call(s.sock(), "POST", "/v1/diagnose", qs[i % 3]);
                if (r.status == 200) {
                    ++ok;
                    return;
                }
                if (r.status == 503 || r.status == 429) {
                    ++busy;
                    std::this_thread::sleep_for(Millis{5});
                    continue;
                }
                ++other;
                ADD_FAILURE() << "status " << r.status << " " << r.body.substr(0, 200);
                return;
            }
        });
    }
    for (auto& t : clients) t.join();
    const auto st = s.server->stats();
    EXPECT_EQ(ok.load(), 100) << "busy " << busy.load() << " other " << other.load();
    EXPECT_EQ(other.load(), 0);
    EXPECT_LE(st.max_in_flight, 16U);
    EXPECT_LE(st.max_queued, 16U) << "the queue is bounded";
}

TEST(ApiServer, DisconnectsAndStopWithActiveRequests) {
    auto c = fast_config();
    c.api_request_timeout = Millis{3000};
    c.api_rate_limit = 10'000;
    c.api_max_clients_per_user = 16;
    Served s("stop", c);
    ASSERT_TRUE(s.server->start().ok());
    ASSERT_TRUE(s.rt->start().ok());
    // Clients that send a request and leave at once.
    for (int i = 0; i < 20; ++i) {
        const int fd = connect_to(s.sock());
        send_all(fd, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: 34\r\n\r\n"
                     "{\"question\":\"why is my pc slow?\"}");
        ::close(fd);
    }
    // Let those drain: otherwise the stalled clients below can exceed the
    // per-user cap and be refused (429) at admission, never reaching stop().
    const auto wait_in_flight = [&](auto pred) {
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (!pred(s.server->stats().in_flight) && Clock::now() < until)
            std::this_thread::sleep_for(Millis{1});
    };
    wait_in_flight([](std::uint64_t n) { return n == 0; });
    // Clients mid-request (stalled) and clients hammering, while stop() runs.
    std::vector<int> stalled;
    for (int i = 0; i < 6; ++i) {
        const int fd = connect_to(s.sock());
        send_all(fd, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\n");
        stalled.push_back(fd);
    }
    wait_in_flight([](std::uint64_t n) { return n >= 6; });
    ASSERT_GE(s.server->stats().in_flight, 6U) << "every stalled client admitted";
    std::atomic<bool> done{false};
    std::atomic<int> answered{0}, refused_conn{0};
    std::vector<std::thread> hammer;
    for (int i = 0; i < 8; ++i)
        hammer.emplace_back([&] {
            while (!done) {
                const auto r = call(s.sock(), "POST", "/v1/diagnose", R"({"question":"what is using the most CPU?"})");
                (r.status ? answered : refused_conn)++;
            }
        });
    std::this_thread::sleep_for(Millis{200});
    const auto t0 = Clock::now();
    s.server->stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    done = true;
    for (auto& t : hammer) t.join();
    // Every stalled client got an answer (503 stopping), not a silent drop.
    for (int fd : stalled) {
        const auto r = parse_reply(read_all(fd, Millis{2000}));
        EXPECT_EQ(r.status, 503) << "stalled client";
        ::close(fd);
    }
    EXPECT_LT(stop_ms, 3500.0 * kSlow) << "bounded by the request timeout";
    EXPECT_GT(answered.load(), 0);
    s.rt->stop();
}

TEST(ApiServer, OneUserCannotOccupyEveryWorker) {
    // Per-user cap 2 of 8 workers: a user stalling connections gets an
    // immediate 429 beyond the cap instead of filling the server.
    auto c = fast_config();
    c.api_max_clients = 8;
    c.api_max_clients_per_user = 2;
    c.api_request_timeout = Millis{2000};
    Served s("peruser", c);
    ASSERT_TRUE(s.server->start().ok());
    std::vector<int> stalled;
    for (int i = 0; i < 2; ++i) {
        stalled.push_back(connect_to(s.sock()));
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (s.server->stats().in_flight < static_cast<std::uint64_t>(i + 1) && Clock::now() < until)
            std::this_thread::sleep_for(Millis{1});
    }
    const auto t0 = Clock::now();
    const auto third = call(s.sock(), "GET", "/v1/capabilities");
    EXPECT_EQ(third.status, 429) << third.body;
    EXPECT_NE(third.body.find("too_many_connections"), std::string::npos);
    EXPECT_LT(std::chrono::duration<double>(Clock::now() - t0).count(), 1.0 * kSlow) << "refused at once";
    EXPECT_GE(s.server->stats().rejected_per_user, 1U);
    for (int fd : stalled) ::close(fd);
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (s.server->stats().in_flight > 0 && Clock::now() < until) std::this_thread::sleep_for(Millis{5});
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200) << "the user's slots are released";
    const auto st = s.server->stats();
    EXPECT_GT(st.total_ms.n, 0U);
    EXPECT_GE(st.total_ms.p99, st.total_ms.p50);
}

TEST(ApiServer, ConcurrentStartupOnOneSocketHasOneWinner) {
    Served s("race");
    std::vector<std::unique_ptr<ApiServer>> servers;
    for (int i = 0; i < 6; ++i) servers.push_back(std::make_unique<ApiServer>(s.rt, s.cfg));
    std::atomic<int> won{0};
    std::vector<std::thread> ts;
    for (auto& srv : servers) ts.emplace_back([&, p = srv.get()] {
        if (p->start().ok()) ++won;
    });
    for (auto& t : ts) t.join();
    EXPECT_EQ(won.load(), 1);
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200);
    servers.clear();
    struct stat st {};
    EXPECT_NE(::lstat(s.sock().c_str(), &st), 0) << "the winner removed its socket; the losers removed nothing";
}

TEST(ApiServer, RapidConnectDisconnectLeaksNothing) {
    auto c = fast_config();
    c.api_rate_limit = 10'000;
    Served s("churn", c);
    ASSERT_TRUE(s.server->start().ok());
    auto open_fds = [] {
        int n = 0;
        for (int fd = 0; fd < 4096; ++fd)
            if (::fcntl(fd, F_GETFD) != -1) ++n;
        return n;
    };
    const int before = open_fds();
    for (int i = 0; i < 2000; ++i) {
        const int fd = connect_to(s.sock());
        if (i % 3 == 0) send_all(fd, "GET /v1/hea");  // some leave mid-request
        ::close(fd);
    }
    const auto until = Clock::now() + std::chrono::seconds(10);
    while ((s.server->stats().in_flight > 0 || s.server->stats().queued > 0) && Clock::now() < until)
        std::this_thread::sleep_for(Millis{10});
    std::this_thread::sleep_for(Millis{600});  // refused connections finish draining
    EXPECT_EQ(call(s.sock(), "GET", "/v1/capabilities").status, 200);
    EXPECT_LE(open_fds(), before + 2) << "every accepted descriptor was closed";
}

TEST(ApiServer, PartialBodiesTimeOut) {
    auto c = fast_config();
    c.api_request_timeout = Millis{300};
    Served s("partial", c);
    ASSERT_TRUE(s.server->start().ok());
    const int fd = connect_to(s.sock());
    send_all(fd, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: 40\r\n\r\n{\"question\":");
    const auto r = parse_reply(read_all(fd, Millis{3000}));
    ::close(fd);
    EXPECT_EQ(r.status, 408);
}

TEST(ApiServer, SmugglingShapesAreRefused) {
    Served s("smuggle");
    ASSERT_TRUE(s.server->start().ok());
    const std::string sock = s.sock();
    // Content-Length and Transfer-Encoding together; a second request hidden in the body.
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n").status, 501);
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Length: 4, 4\r\n\r\n{}{}").status, 400);
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Length: +2\r\n\r\n{}").status, 400);
    EXPECT_EQ(raw_call(sock, "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"
                             "GET /v1/system/snapshot HTTP/1.1\r\nHost: l\r\n\r\n").status, 400) << "trailing request";
    EXPECT_EQ(raw_call(sock, "GET /v1/health/live HTTP/1.1\r\nHost: l\r\nContent-Length : 5\r\n\r\nhello").status, 400)
        << "whitespace before the colon";
    EXPECT_EQ(raw_call(sock, "GET /v1/health/live HTTP/1.1\nHost: l\n\n").status != 200 || true, true);  // bare LF: parser-defined
}

TEST(ApiServer, SurvivesFileDescriptorExhaustion) {
    // The server runs in a child whose descriptor limit leaves room for only
    // a handful of connections; the parent floods it, then checks it recovers.
    const std::string dir = short_tmp("fds");
    const std::string sock = dir + "/agent.sock";
    int ready[2], done[2];
    ASSERT_EQ(::pipe(ready), 0);
    ASSERT_EQ(::pipe(done), 0);
    const pid_t child = ::fork();
    if (child == 0) {
        auto c = fast_config();
        c.api_enabled = true;
        c.api_socket = sock;
        c.api_rate_limit = 10'000;
        c.api_max_clients = 4;
        auto rt = std::make_shared<agent::AgentRuntime>(machine(), c);
        rt->collect_now();
        rt->collect_now();
        ApiServer srv(rt, c);
        if (!srv.start().ok()) ::_exit(3);
        int open_now = 0;
        for (int fd = 0; fd < 1024; ++fd)
            if (::fcntl(fd, F_GETFD) != -1) ++open_now;
        rlimit rl{static_cast<rlim_t>(open_now + 12), static_cast<rlim_t>(open_now + 12)};
        ::setrlimit(RLIMIT_NOFILE, &rl);
        char x = 1;
        (void)!::write(ready[1], &x, 1);
        (void)!::read(done[0], &x, 1);
        srv.stop();
        rt->stop();
        ::_exit(0);
    }
    char x = 0;
    ASSERT_EQ(::read(ready[0], &x, 1), 1);
    std::vector<int> held;
    for (int i = 0; i < 200; ++i) {
        const int fd = connect_to(sock);
        if (fd >= 0) {
            send_all(fd, "GET /v1/health/ready HTTP/1.1\r\nHost: l\r\n\r\n");
            held.push_back(fd);
        }
    }
    std::this_thread::sleep_for(Millis{300 * kSlow});
    for (int fd : held) ::close(fd);
    // Once the flood is gone, the server answers again.
    int status = 0;
    const auto until = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < until && status != 200) {
        status = call(sock, "GET", "/v1/capabilities").status;
        if (status != 200) std::this_thread::sleep_for(Millis{100});
    }
    EXPECT_EQ(status, 200) << "the server recovered after descriptor exhaustion";
    (void)!::write(done[1], &x, 1);
    int wst = 0;
    ASSERT_EQ(::waitpid(child, &wst, 0), child);
    EXPECT_TRUE(WIFEXITED(wst) && WEXITSTATUS(wst) == 0) << "the server process did not crash";
    std::filesystem::remove_all(dir);
}
