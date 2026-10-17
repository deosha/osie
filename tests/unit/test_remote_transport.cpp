// SPDX-License-Identifier: Apache-2.0
// The remote transport with a stand-in for `ssh` (tests/support/fake_ssh.cpp,
// selected with OSIE_SSH) and a real agent API server on a Unix socket:
// end-to-end exchanges, byte-for-byte output, the deadline in every stage,
// classification, and cleanup on every exit path (no zombie, no leaked
// descriptor, no leftover process or directory), including signals sent to
// the `osie` process itself.
#include <gtest/gtest.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "osie/api/server.hpp"
#include "osie/remote/http.hpp"
#include "osie/remote/profile.hpp"
#include "osie/remote/transport.hpp"
#include "support/agent_fixture.hpp"

extern char** environ;

using namespace osie;
using namespace osie::remote;
using namespace std::chrono_literals;
using SClock = std::chrono::steady_clock;

namespace {

#if defined(OSIE_SANITIZED)
constexpr int kSlow = 4;
#else
constexpr int kSlow = 1;
#endif

std::string short_dir(const std::string& tag) {
    const std::string d = "/tmp/osrt" + std::to_string(::getpid() % 100000) + tag;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    ::chmod(d.c_str(), 0700);
    return d;
}

std::size_t entries(const std::string& d) {
    std::size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(d)) {
        (void)e;
        ++n;
    }
    return n;
}

int open_fd_count() {
    int n = 0;
    for (int fd = 0; fd < 4096; ++fd)
        if (::fcntl(fd, F_GETFD) != -1) ++n;
    return n;
}

bool gone(pid_t pid, std::chrono::milliseconds within = 3000ms) {
    const auto until = SClock::now() + within;
    while (SClock::now() < until) {
        if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

std::map<std::string, std::vector<std::string>> read_info(const std::string& path) {
    std::map<std::string, std::vector<std::string>> m;
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)].push_back(line.substr(eq + 1));
    }
    return m;
}

/// Sets environment variables for one test and restores them.
class Env {
public:
    void set(const std::string& k, const std::string& v) {
        save(k);
        ::setenv(k.c_str(), v.c_str(), 1);
    }
    void unset(const std::string& k) {
        save(k);
        ::unsetenv(k.c_str());
    }
    ~Env() {
        for (const auto& [k, v] : saved_) {
            if (v) ::setenv(k.c_str(), v->c_str(), 1);
            else ::unsetenv(k.c_str());
        }
    }

private:
    void save(const std::string& k) {
        if (saved_.count(k) != 0) return;
        const char* v = std::getenv(k.c_str());
        saved_[k] = v ? std::optional<std::string>(v) : std::nullopt;
    }
    std::map<std::string, std::optional<std::string>> saved_;
};

/// A real API server over the mock machine, on a short socket path.
struct Agent {
    explicit Agent(const std::string& tag, bool start_runtime = true) {
        dir = short_dir(tag);
        agent::AgentConfig c = test::fast_config();
        c.api_enabled = true;
        c.api_socket = dir + "/agent.sock";
        c.api_max_evidence_age = Millis{3600 * 1000};
        rt = std::make_shared<agent::AgentRuntime>(test::machine(), c);
        EXPECT_TRUE(rt->collect_now());
        EXPECT_TRUE(rt->collect_now());
        if (start_runtime) EXPECT_TRUE(rt->start().ok());
        server = std::make_unique<api::ApiServer>(rt, c);
        EXPECT_TRUE(server->start().ok());
        sock = c.api_socket;
    }
    ~Agent() {
        server.reset();
        rt->stop();
        std::filesystem::remove_all(dir);
    }
    std::string dir, sock;
    std::shared_ptr<agent::AgentRuntime> rt;
    std::unique_ptr<api::ApiServer> server;
};

/// Direct request on the agent socket (the local baseline).
std::string direct(const std::string& sock, const std::string& req) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, sock.c_str(), sock.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        ::close(fd);
        return {};
    }
    (void)!::write(fd, req.data(), req.size());
    std::string out;
    char buf[65536];
    for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0;) out.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    const auto sep = out.find("\r\n\r\n");
    return sep == std::string::npos ? std::string() : out.substr(sep + 4);
}

/// The per-test fixture: fake ssh, a private runtime directory to inspect.
struct Fixture {
    explicit Fixture(const std::string& tag) {
        runtime = short_dir(tag + "r");
        info = runtime + "-info";
        env.set("XDG_RUNTIME_DIR", runtime);
        env.set("FAKE_SSH_INFO", info);
        env.unset("FAKE_SSH_MODE");
        env.unset("FAKE_SSH_DELAY_MS");
        env.unset("FAKE_SSH_GRANDCHILD");
        env.unset("FAKE_SSH_STDERR");
        env.unset("FAKE_SSH_REPLY");
        env.set("OSIE_SSH", OSIE_FAKE_SSH);
        profile.name = "t";
        profile.destination = "alice@example.test";
        opts.ssh_path = OSIE_FAKE_SSH;
        opts.timeout = std::chrono::milliseconds(5000 * kSlow);
        opts.term_grace = 300ms;
    }
    ~Fixture() {
        std::filesystem::remove_all(runtime);
        std::filesystem::remove(info);
    }
    /// After the tunnel is gone: ssh reaped (not a zombie of ours), the
    /// private directory removed.
    void expect_clean(pid_t pid) const {
        if (pid > 0) {
            int st = 0;
            EXPECT_EQ(::waitpid(pid, &st, WNOHANG), -1) << "ssh was not reaped";
            EXPECT_EQ(errno, ECHILD);
            EXPECT_TRUE(gone(pid)) << "ssh still running";
        }
        EXPECT_EQ(entries(runtime), 0u) << "private directory left behind";
    }
    Env env;
    std::string runtime, info;
    Profile profile;
    TunnelOptions opts;
};

std::string write_reply_file(const std::string& dir, const std::string& name, const std::string& bytes) {
    const std::string p = dir + "/" + name;
    std::ofstream f(p, std::ios::binary);
    f << bytes;
    return p;
}

std::string http_reply(int status, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// ------------------------------------------------------- CLI subprocess ---

struct Run {
    int exit_code{-1};
    int signal{0};
    std::string out, err;
    double ms{0};
};

/// Runs the osie binary with `args`, the current environment plus `extra`.
/// If `signal_after` is set, sends `sig` to it after that delay.
Run run_cli(const std::vector<std::string>& args, const std::map<std::string, std::string>& extra = {},
            std::optional<std::chrono::milliseconds> signal_after = std::nullopt, int sig = SIGTERM,
            bool ignore_sigchld = false) {
    std::vector<std::string> envs;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string kv = *e;
        if (extra.count(kv.substr(0, kv.find('='))) == 0) envs.push_back(kv);
    }
    for (const auto& [k, v] : extra) envs.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::vector<std::string> full = {OSIE_CLI, "remote"};
    if (ignore_sigchld)  // an ignored signal stays ignored across exec
        full = {OSIE_FAKE_SSH, "--exec-ignoring-sigchld", OSIE_CLI, "remote"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : full) argv.push_back(a.data());
    argv.push_back(nullptr);
    int o[2], e[2];
    EXPECT_EQ(::pipe(o), 0);
    EXPECT_EQ(::pipe(e), 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, o[1], 1);
    posix_spawn_file_actions_adddup2(&fa, e[1], 2);
    posix_spawn_file_actions_addclose(&fa, o[0]);
    posix_spawn_file_actions_addclose(&fa, e[0]);
    const auto t0 = SClock::now();
    pid_t pid = -1;
    EXPECT_EQ(posix_spawn(&pid, full[0].c_str(), &fa, nullptr, argv.data(), envp.data()), 0);
    posix_spawn_file_actions_destroy(&fa);
    ::close(o[1]);
    ::close(e[1]);
    Run r;
    bool signalled = false;
    bool o_open = true, e_open = true;
    char buf[65536];
    while (o_open || e_open) {
        pollfd p[2] = {{o_open ? o[0] : -1, POLLIN, 0}, {e_open ? e[0] : -1, POLLIN, 0}};
        int wait_ms = 50;
        (void)::poll(p, 2, wait_ms);
        for (int k = 0; k < 2; ++k) {
            if (!(p[k].revents & (POLLIN | POLLHUP))) continue;
            const auto n = ::read(k == 0 ? o[0] : e[0], buf, sizeof buf);
            if (n <= 0) (k == 0 ? o_open : e_open) = false;
            else (k == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(n));
        }
        if (signal_after && !signalled && SClock::now() - t0 >= *signal_after) {
            ::kill(pid, sig);
            signalled = true;
        }
        if (SClock::now() - t0 > 60s) {
            ::kill(pid, SIGKILL);
            break;
        }
    }
    ::close(o[0]);
    ::close(e[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    r.ms = std::chrono::duration<double, std::milli>(SClock::now() - t0).count();
    if (WIFEXITED(st)) r.exit_code = WEXITSTATUS(st);
    if (WIFSIGNALED(st)) r.signal = WTERMSIG(st);
    return r;
}

/// A profile store with one profile "t" pointing at `socket`.
std::string make_store(const std::string& dir, const std::string& socket, int timeout_s = 5) {
    const std::string cfg = dir + "-cfg";
    std::filesystem::remove_all(cfg);
    std::filesystem::create_directories(cfg);
    ::chmod(cfg.c_str(), 0700);
    ProfileStore store(cfg + "/osie");
    EXPECT_TRUE(store.update([&](std::vector<Profile>& ps) -> Result<void> {
        Profile p;
        p.name = "t";
        p.destination = "alice@example.test";
        p.socket = socket;
        p.timeout_s = timeout_s;
        ps.push_back(p);
        return {};
    }).ok());
    return cfg;
}

}  // namespace

// ------------------------------------------------------------ transport ---

TEST(RemoteTransport, EndToEndAgainstARealAgentServer) {
    Agent agent("e2e");
    Fixture fx("e2e");
    fx.profile.socket = agent.sock;
    const int fds_before = open_fd_count();
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_TRUE(t.ok()) << t.error().message << " / " << t.error().ssh_stderr;
        pid = t.value()->pid();
        // The forwarding socket lives in a fresh 0700 directory of ours.
        const std::string d = t.value()->private_dir();
        EXPECT_EQ(d.rfind(fx.runtime + "/osie-remote-", 0), 0u) << d;
        struct stat st {};
        ASSERT_EQ(::stat(d.c_str(), &st), 0);
        EXPECT_EQ(st.st_mode & 07777, 0700u);
        EXPECT_EQ(st.st_uid, ::geteuid());
        ASSERT_EQ(::lstat((d + "/fwd.sock").c_str(), &st), 0);
        EXPECT_TRUE(S_ISSOCK(st.st_mode));
        EXPECT_EQ(st.st_mode & 0077, 0u) << "StreamLocalBindMask=0177";

        for (const char* target : {"/v1/capabilities", "/v1/system/history?limit=3"}) {
            auto r = t.value()->exchange(http_get(target));
            ASSERT_TRUE(r.ok()) << target << ": " << r.error().message;
            EXPECT_EQ(r.value().status, 200) << target;
        }
        // Byte for byte what the agent sends on its local socket.
        auto caps = t.value()->exchange(http_get("/v1/capabilities"));
        ASSERT_TRUE(caps.ok());
        EXPECT_EQ(caps.value().body, direct(agent.sock, http_get("/v1/capabilities")));
        DiagnoseRequest d1;
        d1.question = "what is using the most CPU?";
        d1.session_id = "remote-s1";
        auto a = t.value()->exchange(http_post_json("/v1/diagnose", diagnose_body(d1)));
        ASSERT_TRUE(a.ok()) << a.error().message;
        EXPECT_EQ(a.value().status, 200);
        EXPECT_NE(a.value().body.find("nginx"), std::string::npos);
        // An API error is an answer, not a transport failure.
        auto bad = t.value()->exchange(http_get("/v1/system/history?limit=0"));
        ASSERT_TRUE(bad.ok());
        EXPECT_EQ(bad.value().status, 400);
        EXPECT_NE(bad.value().body.find("\"invalid_request\""), std::string::npos);
    }
    fx.expect_clean(pid);
    EXPECT_EQ(open_fd_count(), fds_before) << "descriptor leaked";
    // ssh inherited only stdin, stdout and stderr, and its argv ended with
    // "--" and the destination.
    const auto info = read_info(fx.info);
    EXPECT_EQ(info.at("fd"), (std::vector<std::string>{"0", "1", "2"}));
    const auto& args = info.at("arg");
    ASSERT_GE(args.size(), 3u);
    EXPECT_EQ(args[args.size() - 2], "--");
    EXPECT_EQ(args.back(), "alice@example.test");
}

TEST(RemoteTransport, DeadlineWhileWaitingForSsh) {
    Fixture fx("hang");
    fx.env.set("FAKE_SSH_MODE", "hang");
    fx.opts.timeout = std::chrono::milliseconds(700 * kSlow);
    const int fds_before = open_fd_count();
    const auto t0 = SClock::now();
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_FALSE(t.ok());
        EXPECT_EQ(t.error().code, RemoteErrorCode::timeout);
        pid = std::stoi(read_info(fx.info).at("pid").at(0));
    }
    const double ms = std::chrono::duration<double, std::milli>(SClock::now() - t0).count();
    EXPECT_GE(ms, 700.0 * kSlow - 50);
    EXPECT_LT(ms, 700.0 * kSlow + 300 + 500.0 * kSlow) << "deadline + grace";
    fx.expect_clean(pid);
    EXPECT_EQ(open_fd_count(), fds_before);
}

TEST(RemoteTransport, SshIgnoringSigtermIsKilled) {
    Fixture fx("kill");
    fx.env.set("FAKE_SSH_MODE", "hang_ignore_term");
    // Long enough for the stand-in to start and record its pid (slow under sanitizers).
    fx.opts.timeout = std::chrono::milliseconds(400 * kSlow);
    fx.opts.term_grace = 200ms;
    const auto t0 = SClock::now();
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_FALSE(t.ok());
        EXPECT_EQ(t.error().code, RemoteErrorCode::timeout);
        pid = std::stoi(read_info(fx.info).at("pid").at(0));
    }
    const double ms = std::chrono::duration<double, std::milli>(SClock::now() - t0).count();
    EXPECT_GE(ms, 400.0 * kSlow + 200 - 50) << "SIGKILL only after the grace";
    EXPECT_LT(ms, 400.0 * kSlow + 200 + 500.0 * kSlow);
    fx.expect_clean(pid);
}

TEST(RemoteTransport, ProcessGroupIsTerminated) {
    Agent agent("grp");
    Fixture fx("grp");
    fx.profile.socket = agent.sock;
    fx.env.set("FAKE_SSH_GRANDCHILD", "1");
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_TRUE(t.ok()) << t.error().message;
        pid = t.value()->pid();
        EXPECT_EQ(::getpgid(pid), pid) << "ssh leads its own process group";
    }
    fx.expect_clean(pid);
    const pid_t grandchild = std::stoi(read_info(fx.info).at("grandchild").at(0));
    ASSERT_GT(grandchild, 0);
    EXPECT_TRUE(gone(grandchild)) << "a process ssh started (e.g. ProxyCommand) survived";
}

TEST(RemoteTransport, EarlySshExitIsClassified) {
    const std::vector<std::pair<std::string, RemoteErrorCode>> cases = {
        {"No ED25519 host key is known for [h]:22 and you have requested strict checking.\nHost key verification failed.\n",
         RemoteErrorCode::host_key_unknown},
        {"@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @\nHost key for h has changed and you have requested strict "
         "checking.\nHost key verification failed.\n",
         RemoteErrorCode::host_key_mismatch},
        {"alice@h: Permission denied (publickey).\n", RemoteErrorCode::authentication_failed},
        {"ssh: Could not resolve hostname h: Name or service not known\n", RemoteErrorCode::connection_failed},
    };
    for (const auto& [err, code] : cases) {
        Fixture fx("exit");
        fx.env.set("FAKE_SSH_MODE", "exit");
        fx.env.set("FAKE_SSH_STDERR", err);
        fx.env.set("FAKE_SSH_DELAY_MS", "30");
        pid_t pid = -1;
        {
            auto t = SshTunnel::open(fx.profile, fx.opts);
            ASSERT_FALSE(t.ok());
            EXPECT_EQ(t.error().code, code) << err;
            EXPECT_EQ(t.error().ssh_exit, 255);
            EXPECT_FALSE(t.error().ssh_stderr.empty());
            pid = std::stoi(read_info(fx.info).at("pid").at(0));
        }
        fx.expect_clean(pid);
    }
}

TEST(RemoteTransport, ChannelRefusalIsRemoteSocketUnavailable) {
    Fixture fx("eof");
    fx.env.set("FAKE_SSH_MODE", "eof");
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_TRUE(t.ok()) << t.error().message;
        pid = t.value()->pid();
        auto r = t.value()->exchange(http_get("/v1/health/ready"));
        ASSERT_FALSE(r.ok());
        EXPECT_EQ(r.error().code, RemoteErrorCode::remote_socket_unavailable);
        EXPECT_NE(r.error().ssh_stderr.find("open failed"), std::string::npos);
        EXPECT_EQ(::kill(pid, 0), 0) << "ssh is still up (ExitOnForwardFailure does not apply per connection)";
    }
    fx.expect_clean(pid);
}

TEST(RemoteTransport, AgentThatNeverAnswersHitsTheDeadline) {
    Fixture fx("stall");
    fx.env.set("FAKE_SSH_MODE", "stall");
    fx.opts.timeout = 800ms;
    const auto t0 = SClock::now();
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_TRUE(t.ok()) << t.error().message;
        pid = t.value()->pid();
        auto r = t.value()->exchange(http_get("/v1/health/ready"));
        ASSERT_FALSE(r.ok());
        EXPECT_EQ(r.error().code, RemoteErrorCode::timeout);
    }
    EXPECT_LT((std::chrono::duration<double, std::milli>(SClock::now() - t0).count()), 800 + 300 + 500.0 * kSlow);
    fx.expect_clean(pid);
}

TEST(RemoteTransport, HostileRepliesAreProtocolErrors) {
    const std::string body = R"({"schema_version":"1.0"})";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"garbage", "this is not http\r\n\r\n"},
        {"truncated", http_reply(200, body).substr(0, 60)},
        {"chunked", "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n18\r\n" + body +
                        "\r\n0\r\n\r\n"},
        {"huge", "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 5000000\r\n\r\n" +
                     std::string(5'000'000, ' ')},
        {"unbounded", "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 10\r\n\r\n" +
                          std::string(5'000'000, 'x')},
        {"not_agent", http_reply(200, R"({"hello":"world"})")},
        {"html", "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 2\r\n\r\nhi"},
    };
    for (const auto& [name, bytes] : cases) {
        Fixture fx("bad");
        fx.env.set("FAKE_SSH_MODE", "reply_file");
        std::filesystem::create_directories(fx.runtime + "-info-dir");
        fx.env.set("FAKE_SSH_REPLY", write_reply_file(fx.runtime + "-info-dir", name, bytes));
        pid_t pid = -1;
        {
            auto t = SshTunnel::open(fx.profile, fx.opts);
            ASSERT_TRUE(t.ok()) << t.error().message;
            pid = t.value()->pid();
            auto r = t.value()->exchange(http_get("/v1/health/ready"));
            ASSERT_FALSE(r.ok()) << name;
            EXPECT_EQ(r.error().code, RemoteErrorCode::protocol_error) << name << ": " << r.error().message;
        }
        fx.expect_clean(pid);
        std::filesystem::remove_all(fx.runtime + "-info-dir");
    }
}

TEST(RemoteTransport, ForwardPathThatIsNotASocketIsRefused) {
    Fixture fx("nosock");
    fx.env.set("FAKE_SSH_MODE", "not_socket");
    pid_t pid = -1;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_FALSE(t.ok());
        EXPECT_EQ(t.error().code, RemoteErrorCode::protocol_error);
        pid = std::stoi(read_info(fx.info).at("pid").at(0));
    }
    fx.expect_clean(pid);
}

TEST(RemoteTransport, CancellationStopsPromptly) {
    Fixture fx("cancel");
    fx.env.set("FAKE_SSH_MODE", "hang");
    fx.opts.timeout = 30s;
    int p[2];
    ASSERT_EQ(::pipe(p), 0);
    fx.opts.cancel_fd = p[0];
    // Cancel once the stand-in ssh has started (it starts slowly under sanitizers).
    SClock::time_point cancelled_at{};
    std::thread canceller([&, w = p[1]] {
        const auto until = SClock::now() + 10s;
        while (read_info(fx.info).count("pid") == 0 && SClock::now() < until) std::this_thread::sleep_for(10ms);
        std::this_thread::sleep_for(100ms);
        cancelled_at = SClock::now();
        (void)!::write(w, "x", 1);
    });
    std::optional<RemoteError> err;
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        if (!t.ok()) err = t.error();
    }
    const auto done = SClock::now();
    canceller.join();
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, RemoteErrorCode::interrupted);
    const auto info = read_info(fx.info);
    ASSERT_EQ(info.count("pid"), 1u);
    const pid_t pid = std::stoi(info.at("pid").at(0));
    EXPECT_LT((std::chrono::duration<double, std::milli>(done - cancelled_at).count()), 300 + 500.0 * kSlow)
        << "cancellation to cleaned up";
    fx.expect_clean(pid);
    ::close(p[0]);
    ::close(p[1]);
}

TEST(RemoteTransport, MissingSshIsSshUnavailable) {
    Fixture fx("nossh");
    fx.opts.ssh_path = "/nonexistent/bin/ssh";
    const int fds_before = open_fd_count();
    {
        auto t = SshTunnel::open(fx.profile, fx.opts);
        ASSERT_FALSE(t.ok());
        EXPECT_EQ(t.error().code, RemoteErrorCode::ssh_unavailable);
    }
    EXPECT_EQ(entries(fx.runtime), 0u);
    EXPECT_EQ(open_fd_count(), fds_before);
}

TEST(RemoteTransport, PrivateDirectoryFallsBackAndIsUnpredictable) {
    Env env;
    // An unusable XDG_RUNTIME_DIR (world-writable, not sticky) is skipped.
    const std::string bad = short_dir("loose");
    ::chmod(bad.c_str(), 0777);
    env.set("XDG_RUNTIME_DIR", bad);
    const std::string tmp = short_dir("tmpd");
    env.set("TMPDIR", tmp + "/");
    auto a = PrivateDir::create();
    auto b = PrivateDir::create();
    ASSERT_TRUE(a.ok() && b.ok());
    EXPECT_EQ(a.value()->path().rfind(tmp + "/osie-remote-", 0), 0u) << a.value()->path();
    EXPECT_NE(a.value()->path(), b.value()->path());
    EXPECT_EQ(a.value()->path().size(), tmp.size() + 1 + 12 + 24);
    EXPECT_EQ(entries(bad), 0u);
    // A path with characters OpenSSH would expand is skipped.
    const std::string odd = "/tmp/osrt%h" + std::to_string(::getpid());
    std::filesystem::create_directories(odd);
    ::chmod(odd.c_str(), 0700);
    env.set("XDG_RUNTIME_DIR", odd);
    auto c = PrivateDir::create();
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value()->path().find('%'), std::string::npos);
    // Content is removed with the directory.
    const std::string pa = a.value()->path();
    std::ofstream(pa + "/fwd.sock") << "x";
    a.value().reset();
    EXPECT_FALSE(std::filesystem::exists(pa));
    b.value().reset();
    c.value().reset();
    std::filesystem::remove_all(bad);
    std::filesystem::remove_all(tmp);
    std::filesystem::remove_all(odd);
}

// ------------------------------------------------------------------ CLI ---

TEST(RemoteCli, JsonOutputIsTheAgentBodyByteForByte) {
    // A body with unknown fields, odd spacing, escapes and no trailing newline.
    const std::string body = "{ \"schema_version\":\"1.0\" ,\"status\":\"completed\",\"answer\":{\"text\":\"caf\\u00e9\\n\"},"
                             "\"future_feature4_field\":{\"evidence_id\":\"e1\",\"n\":1.50e+3},\"limitations\":[] }";
    const std::string dir = short_dir("bfb");
    const std::string cfg = make_store(dir, "/run/osie/agent.sock");
    const std::string reply = write_reply_file(dir, "reply", http_reply(200, body));
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"ask", "t", "why?", "--json"}, {"snapshot", "t", "--json"}, {"history", "t", "--json", "--limit", "5"},
             {"capabilities", "t", "--json"}}) {
        const auto r = run_cli(args, {{"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir}, {"OSIE_SSH", OSIE_FAKE_SSH},
                                      {"FAKE_SSH_MODE", "reply_file"}, {"FAKE_SSH_REPLY", reply}});
        EXPECT_EQ(r.exit_code, 0) << args[0] << ": " << r.err;
        EXPECT_EQ(r.out, body) << args[0];
    }
    // An API error envelope: exit 1, body unchanged.
    const std::string err_body =
        R"({"schema_version":"1.0","error":{"code":"forbidden","reason":"diagnostics_not_permitted","message":"No.","retryable":false},"action_executed":false})";
    const std::string reply403 = write_reply_file(dir, "reply403", http_reply(403, err_body));
    auto r = run_cli({"ask", "t", "why?", "--json"}, {{"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir},
                                                     {"OSIE_SSH", OSIE_FAKE_SSH}, {"FAKE_SSH_MODE", "reply_file"},
                                                     {"FAKE_SSH_REPLY", reply403}});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_EQ(r.out, err_body);
    r = run_cli({"ask", "t", "why?"}, {{"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir}, {"OSIE_SSH", OSIE_FAKE_SSH},
                                      {"FAKE_SSH_MODE", "reply_file"}, {"FAKE_SSH_REPLY", reply403}});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_EQ(r.err, "agent error 403: forbidden (diagnostics_not_permitted): No.\n");
    // Text mode reads only what it displays.
    r = run_cli({"ask", "t", "why?"}, {{"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir}, {"OSIE_SSH", OSIE_FAKE_SSH},
                                      {"FAKE_SSH_MODE", "reply_file"}, {"FAKE_SSH_REPLY", reply}});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.out, "caf\xc3\xa9\n");
    EXPECT_EQ(entries(dir), 2u) << "only the reply files and nothing of the transport remain";
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(cfg);
}

TEST(RemoteCli, ExitCodesSeparateAnswersTransportAndUsage) {
    Agent agent("cliag");
    const std::string dir = short_dir("cli");
    const std::string cfg = make_store(dir, agent.sock);
    const std::map<std::string, std::string> base = {
        {"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir}, {"OSIE_SSH", OSIE_FAKE_SSH}};
    auto with = [&](std::map<std::string, std::string> m) {
        m.insert(base.begin(), base.end());
        return m;
    };
    auto r = run_cli({"status", "t"}, base);
    EXPECT_EQ(r.exit_code, 0) << r.err;
    EXPECT_NE(r.out.find("agent:   ready (HTTP 200)"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("access:  diagnostics"), std::string::npos) << r.out;
    r = run_cli({"status", "t", "--json"}, base);
    EXPECT_EQ(r.exit_code, 0);
    auto v = parse_json(r.out);
    ASSERT_TRUE(v) << r.out;
    EXPECT_EQ(v->get("ready")->get("http_status")->text, "200");
    EXPECT_EQ(v->get("capabilities")->get("body")->kind, JsonNode::Kind::object);
    r = run_cli({"ask", "t", "what", "is", "using", "the", "most", "CPU?"}, base);
    EXPECT_EQ(r.exit_code, 0) << r.err;
    EXPECT_NE(r.out.find("nginx"), std::string::npos) << r.out;
    r = run_cli({"history", "t", "--limit", "0"}, base);
    EXPECT_EQ(r.exit_code, 1) << "the agent refused the parameter: an answer";
    r = run_cli({"snapshot", "t", "--json"}, with({{"FAKE_SSH_MODE", "eof"}}));
    EXPECT_EQ(r.exit_code, 3);
    v = parse_json(r.out);
    ASSERT_TRUE(v) << r.out;
    EXPECT_EQ(*v->get("remote_error")->str("code"), "remote_socket_unavailable");
    r = run_cli({"snapshot", "t", "--json"},
                with({{"FAKE_SSH_MODE", "exit"}, {"FAKE_SSH_STDERR", "u@h: Permission denied (publickey).\n"}}));
    EXPECT_EQ(r.exit_code, 3);
    EXPECT_NE(r.out.find("\"authentication_failed\""), std::string::npos);
    r = run_cli({"snapshot", "t", "--timeout", "1"}, with({{"FAKE_SSH_MODE", "hang"}}));
    EXPECT_EQ(r.exit_code, 3);
    EXPECT_NE(r.err.find("timeout"), std::string::npos);
    EXPECT_LT(r.ms, 1000 + 500 + 1500.0 * kSlow);
    r = run_cli({"snapshot", "t"}, with({{"OSIE_SSH", "/nonexistent/ssh"}}));
    EXPECT_EQ(r.exit_code, 3);
    EXPECT_NE(r.err.find("ssh_unavailable"), std::string::npos);
    // Usage and profile errors: 2.
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"ask", "t"}, {"ask", "nosuch", "q"}, {"snapshot", "t", "-oProxyCommand=x"}, {"snapshot", "t", "--timeout", "0"},
             {"snapshot", "t", "--timeout", "121"}, {"history", "t", "--metric", "a&b=c"}, {"frobnicate"},
             {"add", "x", "-oProxyCommand=x"}, {"add", "x", "h", "--socket", "relative"}, {"snapshot", "../t"}})
        EXPECT_EQ(run_cli(args, base).exit_code, 2) << args[0] << " " << args.back();
    r = run_cli({"ask", "nosuch", "q", "--json"}, base);
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_NE(r.out.find("\"invalid_profile\""), std::string::npos);
    EXPECT_EQ(entries(dir), 0u) << "no private directory left by any command";
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(cfg);
}

TEST(RemoteCli, InheritedIgnoredSigchldDoesNotLoseSshStatus) {
    // A parent that ignores SIGCHLD passes that on through exec; osie must
    // still reap ssh and classify its exit status.
    const std::string dir = short_dir("chld");
    const std::string cfg = make_store(dir, "/run/osie/agent.sock");
    const auto r = run_cli({"snapshot", "t", "--json"},
                           {{"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir}, {"OSIE_SSH", OSIE_FAKE_SSH},
                            {"FAKE_SSH_MODE", "exit"}, {"FAKE_SSH_DELAY_MS", "50"},
                            {"FAKE_SSH_STDERR", "alice@h: Permission denied (publickey).\n"}},
                           std::nullopt, SIGTERM, /*ignore_sigchld=*/true);
    EXPECT_EQ(r.exit_code, 3);
    EXPECT_NE(r.out.find("\"authentication_failed\""), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("\"ssh_exit_status\":255"), std::string::npos) << r.out;
    EXPECT_EQ(entries(dir), 0u);
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(cfg);
}

TEST(RemoteCli, AddListRemove) {
    const std::string dir = short_dir("alr");
    const std::map<std::string, std::string> env = {{"XDG_CONFIG_HOME", dir}};
    EXPECT_EQ(run_cli({"add", "web", "ops@[::1]", "--port", "2222", "--known-hosts", "/k/kh", "--timeout", "9"}, env).exit_code, 0);
    EXPECT_EQ(run_cli({"add", "web", "other"}, env).exit_code, 2) << "no silent replacement";
    auto r = run_cli({"list", "--json"}, env);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.out,
              "{\"schema_version\":\"1.0\",\"remotes\":[{\"name\":\"web\",\"destination\":\"ops@[::1]\",\"port\":2222,"
              "\"socket\":\"/run/osie/agent.sock\",\"known_hosts\":\"/k/kh\",\"timeout_s\":9}]}\n");
    EXPECT_EQ(run_cli({"remove", "web"}, env).exit_code, 0);
    EXPECT_EQ(run_cli({"remove", "web"}, env).exit_code, 2);
    struct stat st {};
    ASSERT_EQ(::stat((dir + "/osie/remotes.conf").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0600u);
    std::filesystem::remove_all(dir);
}

TEST(RemoteCli, SignalsStopSshAndRemoveTheDirectory) {
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) {
        const std::string dir = short_dir("sig");
        const std::string cfg = make_store(dir, "/run/osie/agent.sock", 30);
        const std::string info = dir + "-info";
        const auto r = run_cli({"snapshot", "t", "--json"},
                               {{"XDG_CONFIG_HOME", cfg}, {"XDG_RUNTIME_DIR", dir}, {"OSIE_SSH", OSIE_FAKE_SSH},
                                {"FAKE_SSH_MODE", "hang_ignore_term"}, {"FAKE_SSH_GRANDCHILD", "1"}, {"FAKE_SSH_INFO", info}},
                               std::chrono::milliseconds(400 * kSlow), sig);
        EXPECT_EQ(r.signal, sig) << "the signal is re-raised after cleanup; exit " << r.exit_code << " " << r.err;
        EXPECT_LT(r.ms, 400.0 * kSlow + 500 + 1500.0 * kSlow);
        EXPECT_NE(r.out.find("\"interrupted\""), std::string::npos) << r.out;
        const auto m = read_info(info);
        ASSERT_TRUE(m.count("pid")) << "fake ssh did not start";
        EXPECT_TRUE(gone(std::stoi(m.at("pid").at(0)))) << "ssh survived signal " << sig;
        EXPECT_TRUE(gone(std::stoi(m.at("grandchild").at(0)))) << "ssh's child survived signal " << sig;
        EXPECT_EQ(entries(dir), 0u) << "private directory left after signal " << sig;
        std::filesystem::remove_all(dir);
        std::filesystem::remove_all(cfg);
        std::filesystem::remove(info);
    }
}
