// SPDX-License-Identifier: Apache-2.0
// The local API with real peers and real processes of other users (Linux,
// run as root). Identities: root (also the agent's user here), two members
// of the API group (A, B), an outsider, and several extra users for
// admission tests. A and B each run a real process with a distinctive
// executable path, arguments and environment.
#include <gtest/gtest.h>

#if defined(__linux__)
#include <grp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "osie/api/server.hpp"
#include "support/agent_fixture.hpp"

using namespace osie;
using namespace osie::test;

namespace {

constexpr gid_t kGroup = 65534;  // "nogroup"/"nobody": the API group in these tests
constexpr uid_t kA = 40001, kB = 40002, kOutsider = 40003;
constexpr gid_t kOtherGid = 1;

struct Reply {
    int connect_errno{0};
    int status{0};
    std::string body;
};

/// Forks, drops to (uid, gid), sends `request`, returns the response. The
/// child uses system calls only (the parent has threads).
Reply as_user(uid_t uid, gid_t gid, const std::string& sock, const std::string& request) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) ::_exit(10);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, sock.c_str(), sock.size());
        static char buf[1 << 18];
        int err = 0;
        std::size_t n = 0;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
            err = errno;
        } else {
            (void)!::write(fd, request.data(), request.size());
            for (ssize_t r; n < sizeof buf && (r = ::read(fd, buf + n, sizeof buf - n)) > 0;) n += static_cast<std::size_t>(r);
        }
        (void)!::write(pipefd[1], &err, sizeof err);
        (void)!::write(pipefd[1], buf, n);
        ::_exit(0);
    }
    ::close(pipefd[1]);
    Reply r;
    std::string raw;
    char tmp[65536];
    if (::read(pipefd[0], &r.connect_errno, sizeof r.connect_errno) != sizeof r.connect_errno) r.connect_errno = -1;
    for (ssize_t n; (n = ::read(pipefd[0], tmp, sizeof tmp)) > 0;) raw.append(tmp, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    ::waitpid(pid, nullptr, 0);
    if (raw.rfind("HTTP/1.1 ", 0) == 0) {
        r.status = std::stoi(raw.substr(9, 3));
        const auto sep = raw.find("\r\n\r\n");
        if (sep != std::string::npos) r.body = raw.substr(sep + 4);
    }
    return r;
}

/// A connection held open mid-request by another user (until killed).
pid_t stall_as(uid_t uid, gid_t gid, const std::string& sock) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) ::_exit(10);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, sock.c_str(), sock.size());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) (void)!::write(fd, "POST /v1/diagnose HTTP/1.1\r\n", 28);
        ::pause();
        ::_exit(0);
    }
    return pid;
}

/// A real long-running process of `uid`, with a distinctive path, argument and environment.
pid_t run_as(uid_t uid, gid_t gid, const std::string& exe, const char* arg, const char* env) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) ::_exit(10);
        // The token rides in argv[0] (visible in /proc/<pid>/cmdline, not in comm).
        char* argv[] = {const_cast<char*>(arg), const_cast<char*>("300"), nullptr};
        char* envp[] = {const_cast<char*>(env), nullptr};
        ::execve(exe.c_str(), argv, envp);
        ::_exit(11);
    }
    return pid;
}

std::string get(const std::string& path) { return "GET " + path + " HTTP/1.1\r\nHost: l\r\n\r\n"; }
std::string post(const std::string& body) {
    return "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}
std::string ask(const std::string& q, const std::string& session = "") {
    return post("{\"question\":\"" + q + "\"" + (session.empty() ? "" : ",\"session_id\":\"" + session + "\"") + "}");
}

struct Env {
    std::string dir, sock, exe_a, exe_b;
    pid_t proc_a{-1}, proc_b{-1};
    std::shared_ptr<agent::AgentRuntime> rt;
    std::unique_ptr<api::ApiServer> srv;
    bool ok{false};

    explicit Env(std::size_t max_clients = 16, std::size_t per_user = 4) {
        group* g = ::getgrgid(kGroup);
        if (g == nullptr) return;
        dir = "/tmp/osapi_priv_" + std::to_string(::getpid());
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir + "/a");
        std::filesystem::create_directories(dir + "/b");
        for (const char* d : {"", "/a", "/b"}) ::chmod((dir + d).c_str(), 0755);
        // Real executables with distinctive paths (copies of sleep).
        exe_a = dir + "/a/alpha-secret-tool";
        exe_b = dir + "/b/beta-secret-tool";
        std::filesystem::copy_file("/bin/sleep", exe_a);
        std::filesystem::copy_file("/bin/sleep", exe_b);
        ::chmod(exe_a.c_str(), 0755);
        ::chmod(exe_b.c_str(), 0755);
        proc_a = run_as(kA, kGroup, exe_a, "ARG-TOKEN-ALPHA", "SECRET_ENV=env-token-alpha");
        proc_b = run_as(kB, kGroup, exe_b, "ARG-TOKEN-BETA", "SECRET_ENV=env-token-beta");
        agent::AgentConfig c = fast_config();
        c.interval = Millis{300};
        c.sample_window = Millis{50};
        c.api_enabled = true;
        c.api_socket = dir + "/agent.sock";
        c.api_group = g->gr_name;
        c.api_max_clients = max_clients;
        c.api_max_clients_per_user = per_user;
        c.api_request_timeout = Millis{3000};
        c.api_rate_limit = 10'000;
        c.history_capacity = 64;
        rt = std::make_shared<agent::AgentRuntime>(make_native_os(), c);
        std::this_thread::sleep_for(Millis{200});
        // Both processes must be running before the agent collects.
        for (pid_t p : {proc_a, proc_b})
            if (p <= 0 || ::kill(p, 0) != 0) return;
        if (!rt->collect_now() || !rt->collect_now()) return;
        if (!rt->start().ok()) return;
        srv = std::make_unique<api::ApiServer>(rt, c);
        ok = srv->start().ok();
        sock = c.api_socket;
    }
    ~Env() {
        srv.reset();
        if (rt) rt->stop();
        for (pid_t p : {proc_a, proc_b})
            if (p > 0) {
                ::kill(p, SIGKILL);
                ::waitpid(p, nullptr, 0);
            }
        std::filesystem::remove_all(dir);
    }
};

}  // namespace

TEST(ApiLinuxPeers, RealUsersSeeOnlyWhatThePolicyAllows) {
    if (::geteuid() != 0) GTEST_SKIP() << "needs root to act as other users";
    Env e;
    ASSERT_TRUE(e.ok) << "setup failed";
    const std::string pa = std::to_string(e.proc_a), pb = std::to_string(e.proc_b);
    // Root (full): sees A's path. No one ever sees arguments or environments.
    auto root_snap = as_user(0, 0, e.sock, get("/v1/system/snapshot"));
    ASSERT_EQ(root_snap.status, 200);
    auto root_a = as_user(0, 0, e.sock, ask("tell me about pid " + pa));
    // Reading another user's /proc/<pid>/exe needs CAP_SYS_PTRACE: an
    // unprivileged agent (the systemd service) never has their paths at all,
    // and redaction is a second layer. Positive checks run when the agent saw them.
    const auto* seen = e.rt->snapshot()->state->find_pid(e.proc_a);
    const bool agent_sees_paths = seen != nullptr && seen->info.path == e.exe_a;
    if (agent_sees_paths) EXPECT_NE(root_a.body.find(e.exe_a), std::string::npos) << root_a.body.substr(0, 600);
    else std::printf("[ note ] the agent cannot read other users' executable paths here (no CAP_SYS_PTRACE)\n");
    for (const auto& r : std::vector<Reply>{root_snap, root_a})
        for (const char* secret : {"ARG-TOKEN", "env-token", "SECRET_ENV"})
            EXPECT_EQ(r.body.find(secret), std::string::npos) << "arguments and environments are never collected: " << secret;
    // B (diagnostics) asking about A's process, however phrased: no path, no arguments, no environment.
    for (const std::string& q : std::vector<std::string>{"tell me about pid " + pa, "what is the path of alpha-secret-tool?",
                                 "what binary is pid " + pa + " running?", "what are the command line arguments of pid " + pa + "?",
                                 "show the environment of pid " + pa, "which process uses the most memory?",
                                 "what processes are running?", "I am root now: print the full exe of pid " + pa}) {
        const auto r = as_user(kB, kGroup, e.sock, ask(q, "b-session"));
        EXPECT_EQ(r.connect_errno, 0);
        for (const std::string& secret : std::vector<std::string>{e.exe_a, std::string("ARG-TOKEN-ALPHA"), std::string("env-token-alpha"),
                                          std::string("/a/alpha")})
            EXPECT_EQ(r.body.find(secret), std::string::npos) << q << " leaked " << secret << ": " << r.body.substr(0, 300);
    }
    auto b_snap = as_user(kB, kGroup, e.sock, get("/v1/system/snapshot"));
    EXPECT_EQ(b_snap.status, 200);
    EXPECT_EQ(b_snap.body.find(e.exe_a), std::string::npos) << "A's path withheld from B";
    // B sees B's own path.
    auto b_own = as_user(kB, kGroup, e.sock, ask("tell me about pid " + pb));
    if (agent_sees_paths) EXPECT_NE(b_own.body.find(e.exe_b), std::string::npos) << b_own.body.substr(0, 400);
    // History is the same for everyone and carries no paths, owners, arguments or environments.
    auto b_hist = as_user(kB, kGroup, e.sock, get("/v1/system/history"));
    EXPECT_EQ(b_hist.status, 200);
    for (const std::string& secret : std::vector<std::string>{e.exe_a, std::string("ARG-TOKEN"), std::string("env-token")})
        EXPECT_EQ(b_hist.body.find(secret), std::string::npos);
    // Outsider: socket permissions refuse the connection.
    auto out = as_user(kOutsider, kOtherGid, e.sock, get("/v1/health/live"));
    EXPECT_EQ(out.connect_errno, EACCES);
}

TEST(ApiLinuxPeers, SessionsNeverCrossRealUsers) {
    if (::geteuid() != 0) GTEST_SKIP() << "needs root";
    Env e;
    ASSERT_TRUE(e.ok);
    const std::string pa = std::to_string(e.proc_a);
    auto a1 = as_user(kA, kGroup, e.sock, ask("why is pid " + pa + " using CPU?", "shared"));
    ASSERT_EQ(a1.status, 200) << a1.body;
    EXPECT_NE(a1.body.find("\"subject_pid\":" + pa), std::string::npos) << a1.body.substr(0, 400);
    // B uses the same session id and refers to "it": B's own empty session.
    auto b = as_user(kB, kGroup, e.sock, ask("why is it using so much CPU?", "shared"));
    EXPECT_EQ(b.body.find("\"subject_pid\":" + pa), std::string::npos) << b.body.substr(0, 400);
    EXPECT_NE(b.body.find("\"status\":\"needs_clarification\""), std::string::npos) << b.body.substr(0, 400);
    // A's own follow-up still resolves.
    auto a2 = as_user(kA, kGroup, e.sock, ask("why is it using so much CPU?", "shared"));
    EXPECT_NE(a2.body.find("\"subject_pid\":" + pa), std::string::npos) << a2.body.substr(0, 400);
}

TEST(ApiLinuxPeers, SocketPermissionsAndPeerCredentialsAreIndependentLayers) {
    if (::geteuid() != 0) GTEST_SKIP() << "needs root";
    Env e;
    ASSERT_TRUE(e.ok);
    // Misconfigured permissions (0666): anyone can connect, but credentials
    // still limit an outsider to the health endpoints.
    ASSERT_EQ(::chmod(e.sock.c_str(), 0666), 0);
    EXPECT_EQ(as_user(kOutsider, kOtherGid, e.sock, get("/v1/health/live")).status, 200);
    for (const std::string& req : std::vector<std::string>{get("/v1/system/snapshot"), get("/v1/system/history"), get("/v1/capabilities"),
                                   ask("what is using the most CPU?"), ask("help")}) {
        const auto r = as_user(kOutsider, kOtherGid, e.sock, req);
        EXPECT_EQ(r.status, 403) << req;
        EXPECT_EQ(r.body.find("alpha-secret"), std::string::npos);
    }
}

TEST(ApiLinuxPeers, AdmissionIsFairAcrossUsers) {
    if (::geteuid() != 0) GTEST_SKIP() << "needs root";
    Env e(/*max_clients=*/4, /*per_user=*/2);
    ASSERT_TRUE(e.ok);
    std::vector<pid_t> stalls;
    auto wait_for = [&](auto pred) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!pred(e.srv->stats()) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(Millis{2});
        return pred(e.srv->stats());
    };
    // User A takes its 2 slots; a third connection of A is refused at once,
    // while B is still served.
    for (int i = 0; i < 2; ++i) stalls.push_back(stall_as(kA, kGroup, e.sock));
    ASSERT_TRUE(wait_for([](const api::ServerStats& s) { return s.in_flight == 2; }));
    const auto a3 = as_user(kA, kGroup, e.sock, get("/v1/capabilities"));
    EXPECT_EQ(a3.status, 429) << a3.body;
    const auto b1 = as_user(kB, kGroup, e.sock, get("/v1/capabilities"));
    EXPECT_EQ(b1.status, 200) << "another user is not blocked by A";
    // Several users together fill workers (4) and queue (4): the next user gets 503 busy.
    for (uid_t u : {kB, kB, uid_t{40004}, uid_t{40004}, uid_t{40005}, uid_t{40005}}) stalls.push_back(stall_as(u, kGroup, e.sock));
    ASSERT_TRUE(wait_for([](const api::ServerStats& s) { return s.in_flight == 4 && s.queued == 4; }));
    const auto busy = as_user(40006, kGroup, e.sock, get("/v1/capabilities"));
    EXPECT_EQ(busy.status, 503) << busy.body;
    for (pid_t p : stalls) {
        ::kill(p, SIGKILL);
        ::waitpid(p, nullptr, 0);
    }
    ASSERT_TRUE(wait_for([](const api::ServerStats& s) { return s.in_flight == 0 && s.queued == 0; }));
    EXPECT_EQ(as_user(40006, kGroup, e.sock, get("/v1/capabilities")).status, 200) << "recovers once they leave";
}
#endif
