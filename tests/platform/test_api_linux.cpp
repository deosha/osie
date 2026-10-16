// SPDX-License-Identifier: Apache-2.0
// The local API with real peers of other users (Linux, run as root): socket
// permissions keep non-members out, and authorisation from SO_PEERCRED holds
// even when the socket's permissions are misconfigured.
#include <gtest/gtest.h>

#if defined(__linux__)
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "osie/api/server.hpp"
#include "support/agent_fixture.hpp"

using namespace osie;
using namespace osie::test;

namespace {

constexpr uid_t kNobody = 65534;
constexpr gid_t kNoGroup = 65534;

struct ChildResult {
    int connect_errno{0};
    int status{0};       // HTTP status, 0 if none
    bool body_has_path{false};
};

/// Forks, drops to (uid, gid, no supplementary groups) and sends `request`.
/// The child uses system calls only (the parent has threads).
ChildResult as_user(uid_t uid, gid_t gid, const std::string& sock, const std::string& request, const char* needle) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        int out[3] = {0, 0, 0};
        if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) ::_exit(10);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, sock.c_str(), sock.size());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
            out[0] = errno;
        } else {
            (void)!::write(fd, request.data(), request.size());
            static char buf[65536];
            std::size_t n = 0;
            for (ssize_t r; n < sizeof buf - 1 && (r = ::read(fd, buf + n, sizeof buf - 1 - n)) > 0;) n += static_cast<std::size_t>(r);
            buf[n] = '\0';
            if (n > 12 && std::memcmp(buf, "HTTP/1.1 ", 9) == 0)
                out[1] = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
            out[2] = std::strstr(buf, needle) != nullptr;
        }
        (void)!::write(pipefd[1], out, sizeof out);
        ::_exit(0);
    }
    ::close(pipefd[1]);
    int out[3] = {0, 0, 0};
    (void)!::read(pipefd[0], out, sizeof out);
    ::close(pipefd[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    return {out[0], out[1], out[2] != 0};
}

std::string get(const std::string& path) { return "GET " + path + " HTTP/1.1\r\nHost: l\r\n\r\n"; }

}  // namespace

TEST(ApiLinuxPeers, SocketPermissionsAndPeerCredentialsBothGuardTheApi) {
    if (::geteuid() != 0) GTEST_SKIP() << "needs root to act as other users";
    group* ng = ::getgrgid(kNoGroup);
    if (ng == nullptr) GTEST_SKIP() << "no group 65534";
    const std::string dir = "/tmp/osapi_peers_" + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    ::chmod(dir.c_str(), 0755);  // others may traverse; the socket's own mode decides
    agent::AgentConfig c = fast_config();
    c.api_enabled = true;
    c.api_socket = dir + "/agent.sock";
    c.api_group = ng->gr_name;
    auto rt = std::make_shared<agent::AgentRuntime>(machine(), c);
    ASSERT_TRUE(rt->collect_now());
    ASSERT_TRUE(rt->collect_now());
    {
        api::ApiServer srv(rt, c);
        auto r = srv.start();
        ASSERT_TRUE(r.ok()) << r.error().describe();
        const std::string snapshot = get("/v1/system/snapshot");
        // A group member: diagnostics, with other users' paths withheld.
        auto member = as_user(kNobody, kNoGroup, c.api_socket, snapshot, "/usr/lib/postgresql");
        EXPECT_EQ(member.connect_errno, 0);
        EXPECT_EQ(member.status, 200);
        EXPECT_FALSE(member.body_has_path) << "another user's executable path";
        // Not a member: the socket's mode (0660) refuses the connection.
        auto outsider = as_user(kNobody, 1, c.api_socket, get("/v1/health/live"), "");
        EXPECT_EQ(outsider.connect_errno, EACCES);
        // Misconfigured permissions (0666): anyone can connect, but peer
        // credentials still limit them to the health endpoints.
        ASSERT_EQ(::chmod(c.api_socket.c_str(), 0666), 0);
        auto open_health = as_user(kNobody, 1, c.api_socket, get("/v1/health/ready"), "");
        EXPECT_EQ(open_health.connect_errno, 0);
        EXPECT_TRUE(open_health.status == 200 || open_health.status == 503) << open_health.status;
        auto open_snapshot = as_user(kNobody, 1, c.api_socket, snapshot, "nginx");
        EXPECT_EQ(open_snapshot.status, 403);
        EXPECT_FALSE(open_snapshot.body_has_path) << "no diagnostic content in the refusal";
        const std::string body = R"({"question":"what is using the most CPU?"})";
        auto open_diag = as_user(kNobody, 1, c.api_socket,
                                 "POST /v1/diagnose HTTP/1.1\r\nHost: l\r\nContent-Type: application/json\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\n\r\n" + body,
                                 "nginx");
        EXPECT_EQ(open_diag.status, 403);
        EXPECT_FALSE(open_diag.body_has_path);
        srv.stop();
    }
    std::filesystem::remove_all(dir);
}
#endif
