// SPDX-License-Identifier: Apache-2.0
// A stand-in for OpenSSH's `ssh` in the remote transport tests, selected
// with OSIE_SSH. It understands only "-L LOCAL:REMOTE" (the rest of the
// command line is recorded, not interpreted) and behaves as FAKE_SSH_MODE
// says:
//   forward        bind LOCAL (umask 0177) and relay each connection to REMOTE
//   eof            accept, print a channel-open failure, close (no remote socket)
//   eof_perm       the same, "Permission denied"
//   exit           print $FAKE_SSH_STDERR, exit $FAKE_SSH_EXIT (default 255)
//   hang           never create the socket
//   hang_ignore_term  the same, ignoring SIGTERM (needs SIGKILL)
//   stall          accept and never answer
//   reply_file     accept, read the request head, send $FAKE_SSH_REPLY raw, close
//   not_socket     create a regular file at LOCAL
// FAKE_SSH_DELAY_MS delays everything; FAKE_SSH_GRANDCHILD=1 first starts a
// long-lived child in the same process group. FAKE_SSH_INFO, if set, is a
// file that receives "pid=<pid>", "grandchild=<pid>", "fd=<n>" for every
// open descriptor at start, and "arg=<a>" for every argument.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string env(const char* k, const char* def = "") {
    const char* v = std::getenv(k);
    return v != nullptr ? v : def;
}

[[noreturn]] void sleep_forever() {
    while (true) ::pause();
}

int listen_at(const std::string& path) {
    ::umask(0177);
    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(s, 16) != 0) {
        std::fprintf(stderr, "unix_listener: cannot bind to path %s: %s\nCould not request local forwarding.\n",
                     path.c_str(), std::strerror(errno));
        std::exit(255);
    }
    return s;
}

int connect_to(const std::string& path) {
    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
    if (::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        ::close(s);
        return -1;
    }
    return s;
}

void relay(int a, const std::string& remote) {
    const int b = connect_to(remote);
    if (b < 0) {
        std::fprintf(stderr, "channel 2: open failed: connect failed: %s\n", std::strerror(errno));
        ::close(a);
        return;
    }
    char buf[65536];
    bool a_open = true, b_open = true;
    while (a_open || b_open) {
        pollfd p[2] = {{a_open ? a : -1, POLLIN, 0}, {b_open ? b : -1, POLLIN, 0}};
        if (::poll(p, 2, -1) <= 0) break;
        if (a_open && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            const auto n = ::read(a, buf, sizeof buf);
            if (n <= 0) {
                a_open = false;
                ::shutdown(b, SHUT_WR);
            } else {
                (void)!::write(b, buf, static_cast<std::size_t>(n));
            }
        }
        if (b_open && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            const auto n = ::read(b, buf, sizeof buf);
            if (n <= 0) {
                b_open = false;
                break;  // the agent closed after its response
            } else {
                (void)!::write(a, buf, static_cast<std::size_t>(n));
            }
        }
    }
    ::close(a);
    ::close(b);
}

}  // namespace

int main(int argc, char** argv) {
    std::string local, remote;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            const std::string spec = argv[i + 1];
            const auto colon = spec.find(':');
            local = spec.substr(0, colon);
            remote = colon == std::string::npos ? "" : spec.substr(colon + 1);
        }
    pid_t grandchild = -1;
    if (env("FAKE_SSH_GRANDCHILD") == "1") {
        grandchild = ::fork();
        if (grandchild == 0) {
            ::execl("/bin/sleep", "sleep", "300", static_cast<char*>(nullptr));
            ::_exit(1);
        }
    }
    if (const std::string info = env("FAKE_SSH_INFO"); !info.empty()) {
        std::vector<int> fds;  // before opening the info file
        for (int fd = 0; fd < 1024; ++fd)
            if (fd != 0 && fd != 1 && fd != 2 && ::fcntl(fd, F_GETFD) != -1) fds.push_back(fd);
        std::ofstream f(info);
        f << "pid=" << ::getpid() << "\n";
        f << "grandchild=" << grandchild << "\n";
        for (int fd = 0; fd < 3; ++fd)
            if (::fcntl(fd, F_GETFD) != -1) f << "fd=" << fd << "\n";
        for (int fd : fds) f << "fd=" << fd << "\n";
        for (int i = 0; i < argc; ++i) f << "arg=" << argv[i] << "\n";
    }
    const std::string mode = env("FAKE_SSH_MODE", "forward");
    if (const int d = std::atoi(env("FAKE_SSH_DELAY_MS", "0").c_str()); d > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(d));
    if (mode == "exit") {
        const std::string e = env("FAKE_SSH_STDERR");
        std::fputs(e.c_str(), stderr);
        std::fflush(stderr);
        return std::atoi(env("FAKE_SSH_EXIT", "255").c_str());
    }
    if (mode == "hang") sleep_forever();
    if (mode == "hang_ignore_term") {
        ::signal(SIGTERM, SIG_IGN);
        sleep_forever();
    }
    if (local.empty()) {
        std::fputs("fake ssh: no -L given\n", stderr);
        return 255;
    }
    if (mode == "not_socket") {
        std::ofstream(local) << "x";
        sleep_forever();
    }
    const int ls = listen_at(local);
    while (true) {
        const int c = ::accept(ls, nullptr, nullptr);
        if (c < 0) continue;
        if (mode == "eof" || mode == "eof_perm") {
            std::fprintf(stderr, "channel 2: open failed: connect failed: %s\n",
                         mode == "eof" ? "No such file or directory" : "Permission denied");
            std::fflush(stderr);
            ::close(c);
        } else if (mode == "stall") {
            std::thread([c] {
                std::this_thread::sleep_for(std::chrono::hours(1));
                ::close(c);
            }).detach();
        } else if (mode == "reply_file") {
            char buf[65536];
            std::string req;
            while (req.find("\r\n\r\n") == std::string::npos) {
                const auto n = ::read(c, buf, sizeof buf);
                if (n <= 0) break;
                req.append(buf, static_cast<std::size_t>(n));
            }
            std::ifstream f(env("FAKE_SSH_REPLY"), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string out = ss.str();
            std::size_t off = 0;
            while (off < out.size()) {
                const auto n = ::write(c, out.data() + off, out.size() - off);
                if (n <= 0) break;
                off += static_cast<std::size_t>(n);
            }
            ::close(c);
        } else {
            std::thread(relay, c, remote).detach();
        }
    }
}
