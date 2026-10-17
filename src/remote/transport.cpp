// SPDX-License-Identifier: Apache-2.0
#include "osie/remote/transport.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "osie/api/peer.hpp"
#include "osie/remote/ssh.hpp"

extern char** environ;

namespace osie::remote {
namespace {

constexpr std::size_t kMaxStderr = 16 * 1024;
constexpr const char* kSocketName = "fwd.sock";

int ms_until(Clock::time_point t) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(t - Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(d, 0, 1'000'000));
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

bool set_cloexec_nonblock(int fd, bool nonblock) {
    const int fdf = ::fcntl(fd, F_GETFD);
    if (fdf < 0 || ::fcntl(fd, F_SETFD, fdf | FD_CLOEXEC) != 0) return false;
    if (nonblock) {
        const int fl = ::fcntl(fd, F_GETFL);
        if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return false;
    }
    return true;
}

/// A pipe whose ends are close-on-exec (atomically on Linux).
bool cloexec_pipe(int fds[2]) {
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0) return false;
    if (!set_cloexec_nonblock(fds[0], false) || !set_cloexec_nonblock(fds[1], false)) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    return true;
#endif
}

bool safe_chars(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
               c == '_' || c == '-';
    });
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> b(bytes);
    arc4random_buf(b.data(), b.size());
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) {
        s += kHex[c >> 4];
        s += kHex[c & 15];
    }
    return s;
}

/// Signals reset to their default action in ssh.
constexpr int kResetSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGCHLD, SIGALRM,
                                 SIGUSR1, SIGUSR2, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH};

/// Spawns `argv` in its own process group with stdin/stdout on /dev/null,
/// stderr on `err_w` and no other descriptor. Returns the pid, or -errno.
pid_t spawn_ssh(const std::vector<std::string>& args, int err_w) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
#if defined(__APPLE__)
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    if (posix_spawn_file_actions_init(&fa) != 0) return -ENOMEM;
    if (posix_spawnattr_init(&at) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -ENOMEM;
    }
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, err_w, 2);
    sigset_t def, none;
    sigemptyset(&def);
    for (int s : kResetSignals) sigaddset(&def, s);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setpgroup(&at, 0);
    // CLOEXEC_DEFAULT: every descriptor not named in the file actions is
    // closed in the child, whatever its flags.
    posix_spawnattr_setflags(&at, static_cast<short>(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK |
                                                     POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT));
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0], &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return rc == 0 ? pid : -rc;
#else
    // fork+execve so the child can ask for SIGTERM when osie dies
    // (PR_SET_PDEATHSIG), which posix_spawn cannot. The child calls only
    // async-signal-safe functions.
    int st[2];
    if (!cloexec_pipe(st)) return -errno;
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(st[0]);
        ::close(st[1]);
        return -e;
    }
    if (pid == 0) {
        int err = 0;
        ::setpgid(0, 0);
        if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent) ::_exit(127);
        const int nul = ::open("/dev/null", O_RDWR);
        if (nul < 0 || ::dup2(nul, 0) < 0 || ::dup2(nul, 1) < 0 || ::dup2(err_w, 2) < 0) {
            err = errno;
            (void)!::write(st[1], &err, sizeof err);
            ::_exit(127);
        }
        struct sigaction sa {};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        for (int s : kResetSignals) ::sigaction(s, &sa, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        // Close everything above stderr except the status pipe (close-on-exec).
        const int keep = st[1];
#if defined(SYS_close_range)
        bool closed = true;
        if (keep > 3 && ::syscall(SYS_close_range, 3U, static_cast<unsigned>(keep - 1), 0U) != 0) closed = false;
        if (closed && ::syscall(SYS_close_range, static_cast<unsigned>(keep + 1), ~0U, 0U) != 0) closed = false;
        if (!closed)
#endif
        {
            const long maxfd = std::min<long>(::sysconf(_SC_OPEN_MAX), 65536);
            for (int fd = 3; fd < maxfd; ++fd)
                if (fd != keep) ::close(fd);
        }
        ::execve(argv[0], argv.data(), environ);
        err = errno;
        (void)!::write(st[1], &err, sizeof err);
        ::_exit(127);
    }
    ::close(st[1]);
    int child_err = 0;
    ssize_t n;
    do n = ::read(st[0], &child_err, sizeof child_err);
    while (n < 0 && errno == EINTR);
    ::close(st[0]);
    if (n == static_cast<ssize_t>(sizeof child_err)) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return -(child_err != 0 ? child_err : ENOEXEC);
    }
    return pid;
#endif
}

std::atomic<int> g_signal_fd_w{-1};
std::atomic<int> g_signal_fd_r{-1};
std::atomic<int> g_caught{0};
struct sigaction g_old[4];
constexpr int kGuarded[] = {SIGINT, SIGTERM, SIGHUP, SIGPIPE};

extern "C" void on_signal(int sig) {
    const int saved = errno;
    int expected = 0;
    g_caught.compare_exchange_strong(expected, sig);
    const int fd = g_signal_fd_w.load();
    if (fd >= 0) {
        const char c = 1;
        (void)!::write(fd, &c, 1);
    }
    errno = saved;
}

}  // namespace

// ---------------------------------------------------------- PrivateDir ---

Result<std::unique_ptr<PrivateDir>> PrivateDir::create() {
    std::vector<std::string> parents;
    const bool setid = ::getuid() != ::geteuid() || ::getgid() != ::getegid();
    if (!setid) {
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR"})
            if (const char* v = std::getenv(var); v != nullptr && v[0] == '/') parents.emplace_back(v);
    }
    parents.emplace_back("/tmp");
    const std::size_t max_path = sizeof(sockaddr_un{}.sun_path) - 1;
    std::string last_error = "no usable directory for the forwarding socket";
    for (std::string parent : parents) {
        while (parent.size() > 1 && parent.back() == '/') parent.pop_back();
        // "<parent>/osie-remote-<24 hex>/fwd.sock"
        if (!safe_chars(parent) || parent.size() + 1 + 12 + 24 + 1 + std::strlen(kSocketName) > max_path) {
            last_error = parent + " is unsuitable for the forwarding socket (characters or length)";
            continue;
        }
        secure::OpenOptions oo;
        oo.what = "forwarding directory";
        auto pfd = secure::open_trusted_dir(parent, oo);
        if (!pfd) {
            last_error = pfd.error().message;
            continue;
        }
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::string name = "osie-remote-" + random_hex(12);
            if (::mkdirat(pfd->get(), name.c_str(), 0700) != 0) {
                if (errno == EEXIST) continue;
                last_error = "cannot create a directory in " + parent + ": " + std::strerror(errno);
                break;
            }
            std::unique_ptr<PrivateDir> d(new PrivateDir());
            d->name_ = name;
            d->path_ = parent + (parent == "/" ? "" : "/") + name;
            d->dir_ = secure::Fd(::openat(pfd->get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            d->parent_ = std::move(pfd).value();
            struct stat st {};
            if (!d->dir_ || ::fchmod(d->dir_.get(), 0700) != 0 || ::fstat(d->dir_.get(), &st) != 0 ||
                st.st_uid != ::geteuid() || (st.st_mode & 07777) != 0700) {
                // Destructor removes it if we did create it.
                return Error{Errc::permission_denied, "the private directory " + d->path_ + " failed its checks", 0};
            }
            return d;
        }
    }
    return Error{Errc::unavailable, last_error, 0};
}

void PrivateDir::remove() {
    if (!parent_) return;
    // Only the user (and root) can create entries in a 0700 directory: remove
    // whatever is there (normally the forwarding socket, if ssh left it).
    const int dup = dir_ ? ::fcntl(dir_.get(), F_DUPFD_CLOEXEC, 0) : -1;
    if (dup >= 0) {
        if (DIR* d = ::fdopendir(dup)) {
            std::vector<std::string> names;
            while (dirent* e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") names.push_back(n);
            }
            ::closedir(d);
            for (const auto& n : names)
                if (::unlinkat(dir_.get(), n.c_str(), 0) != 0 && errno == EPERM)
                    ::unlinkat(dir_.get(), n.c_str(), AT_REMOVEDIR);
        } else {
            ::close(dup);
        }
    }
    ::unlinkat(parent_.get(), name_.c_str(), AT_REMOVEDIR);
    dir_.reset();
    parent_.reset();
}

PrivateDir::~PrivateDir() { remove(); }

// ----------------------------------------------------------- SshTunnel ---

const std::string& SshTunnel::private_dir() const {
    static const std::string kNone;
    return dir_ ? dir_->path() : kNone;
}

bool SshTunnel::cancelled() const {
    if (opt_.cancel_fd < 0) return false;
    pollfd p{opt_.cancel_fd, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP)) != 0;
}

void SshTunnel::drain_stderr(std::chrono::milliseconds wait) {
    if (!err_) return;
    const auto until = Clock::now() + wait;
    char buf[4096];
    while (true) {
        const auto n = ::read(err_.get(), buf, sizeof buf);
        if (n > 0) {
            const std::size_t room = kMaxStderr - std::min(kMaxStderr, stderr_.size());
            stderr_.append(buf, std::min(room, static_cast<std::size_t>(n)));
            continue;
        }
        if (n == 0) {  // ssh (and anything sharing its stderr) closed it
            err_.reset();
            return;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            err_.reset();
            return;
        }
        const int left = ms_until(until);
        if (left <= 0) return;
        pollfd p{err_.get(), POLLIN, 0};
        if (::poll(&p, 1, left) <= 0) return;
    }
}

bool SshTunnel::reap_nonblocking() {
    if (reaped_ || pid_ <= 0) return reaped_;
    int st = 0;
    pid_t r;
    do r = ::waitpid(pid_, &st, WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == pid_) {
        reaped_ = true;
        wait_status_ = st;
    } else if (r < 0 && errno == ECHILD) {
        reaped_ = true;  // cannot happen unless someone else reaped it
    }
    return reaped_;
}

RemoteError SshTunnel::exited_error() {
    drain_stderr(std::chrono::milliseconds(200));
    if (WIFSIGNALED(wait_status_)) {
        RemoteError e;
        e.code = RemoteErrorCode::connection_failed;
        e.ssh_signal = WTERMSIG(wait_status_);
        e.ssh_stderr = sanitize_stderr(stderr_);
        e.message = "ssh was terminated by signal " + std::to_string(WTERMSIG(wait_status_)) + ".";
        return e;
    }
    return classify_ssh_exit(WIFEXITED(wait_status_) ? WEXITSTATUS(wait_status_) : 255, stderr_);
}

RemoteError SshTunnel::timeout_error(const char* stage) {
    drain_stderr(std::chrono::milliseconds(0));
    RemoteError e;
    e.code = RemoteErrorCode::timeout;
    e.message = "No answer within " + std::to_string(opt_.timeout.count() / 1000) + " s (" + stage + ").";
    e.ssh_stderr = sanitize_stderr(stderr_);
    return e;
}

RemoteError SshTunnel::interrupted_error() {
    RemoteError e;
    e.code = RemoteErrorCode::interrupted;
    e.message = "Interrupted by a signal; ssh was stopped and the forwarding socket removed.";
    return e;
}

Outcome<std::unique_ptr<SshTunnel>> SshTunnel::open(const Profile& p, const TunnelOptions& o) {
    std::unique_ptr<SshTunnel> t(new SshTunnel());
    t->opt_ = o;
    t->start_ = Clock::now();
    t->deadline_ = t->start_ + o.timeout;

    auto dir = PrivateDir::create();
    if (!dir) {
        RemoteError e;
        e.code = RemoteErrorCode::connection_failed;
        e.message = "Cannot create a private directory for the forwarding socket: " + dir.error().message;
        return e;
    }
    t->dir_ = std::move(dir).value();
    const auto remaining_s = static_cast<int>((ms_until(t->deadline_) + 999) / 1000);
    auto argv = build_ssh_argv(o.ssh_path, p, t->dir_->socket_path(), std::max(1, remaining_s));
    if (!argv) {
        RemoteError e;
        e.code = RemoteErrorCode::invalid_profile;
        e.message = argv.error().message;
        return e;
    }
    int ep[2];
    if (!cloexec_pipe(ep)) {
        RemoteError e;
        e.code = RemoteErrorCode::connection_failed;
        e.message = std::string("cannot create a pipe: ") + std::strerror(errno);
        return e;
    }
    t->err_ = secure::Fd(ep[0]);
    secure::Fd err_w(ep[1]);
    set_cloexec_nonblock(ep[0], true);
    const pid_t pid = spawn_ssh(*argv, err_w.get());
    err_w.reset();
    if (pid < 0) {
        RemoteError e;
        e.code = RemoteErrorCode::ssh_unavailable;
        e.message = "Cannot run " + o.ssh_path + ": " + std::strerror(-pid);
        return e;
    }
    t->pid_ = pid;

    // Wait for the forwarding socket.
    int step_ms = 1;
    while (true) {
        if (t->cancelled()) return t->interrupted_error();
        t->drain_stderr(std::chrono::milliseconds(0));
        struct stat st {};
        if (::fstatat(t->dir_->fd(), kSocketName, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid()) {
                RemoteError e;
                e.code = RemoteErrorCode::protocol_error;
                e.message = "The forwarding path is not a socket owned by the user; refusing to use it.";
                return e;
            }
            break;
        }
        if (t->reap_nonblocking()) return t->exited_error();
        const int left = ms_until(t->deadline_);
        if (left <= 0) return t->timeout_error("waiting for SSH to connect and authenticate");
        pollfd fds[2];
        nfds_t n = 0;
        if (t->err_) fds[n++] = pollfd{t->err_.get(), POLLIN, 0};
        if (o.cancel_fd >= 0) fds[n++] = pollfd{o.cancel_fd, POLLIN, 0};
        (void)::poll(fds, n, std::min(left, step_ms));
        step_ms = std::min(step_ms * 2, 20);
    }
    t->ready_ms_ = ms_since(t->start_);
    return t;
}

Outcome<HttpReply> SshTunnel::exchange(const std::string& request) {
    if (!dir_ || pid_ <= 0) {
        RemoteError e;
        e.code = RemoteErrorCode::connection_failed;
        e.message = "The SSH connection is closed.";
        return e;
    }
    const std::string path = dir_->socket_path();
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, path.c_str(), path.size() + 1);

    // Connect. Between bind() and listen() in ssh a connect is refused: retry.
    secure::Fd s;
    int step_ms = 1;
    while (true) {
        if (cancelled()) return interrupted_error();
        s = secure::Fd(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!s || !set_cloexec_nonblock(s.get(), true)) {
            RemoteError e;
            e.code = RemoteErrorCode::connection_failed;
            e.message = std::string("cannot create a socket: ") + std::strerror(errno);
            return e;
        }
#if defined(SO_NOSIGPIPE)
        const int one = 1;
        ::setsockopt(s.get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (::connect(s.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) break;
        if (errno == EINPROGRESS || errno == EAGAIN) {
            pollfd p{s.get(), POLLOUT, 0};
            if (::poll(&p, 1, ms_until(deadline_)) > 0) {
                int soerr = 0;
                socklen_t len = sizeof soerr;
                ::getsockopt(s.get(), SOL_SOCKET, SO_ERROR, &soerr, &len);
                if (soerr == 0) break;
            }
        }
        if (reap_nonblocking()) return exited_error();
        if (ms_until(deadline_) <= 0) return timeout_error("connecting to the forwarding socket");
        pollfd p{opt_.cancel_fd, POLLIN, 0};
        (void)::poll(&p, opt_.cancel_fd >= 0 ? 1 : 0, std::min(ms_until(deadline_), step_ms));
        step_ms = std::min(step_ms * 2, 20);
    }

    // The listener must be ssh: same uid, and its pid where the kernel says.
    const auto peer = api::peer_of(s.get());
    if (!peer.known || peer.uid != ::geteuid() || (peer.pid && *peer.pid != pid_)) {
        RemoteError e;
        e.code = RemoteErrorCode::protocol_error;
        e.message = "The forwarding socket is not served by the ssh process; refusing to use it.";
        return e;
    }

    // Send.
    std::size_t off = 0;
    while (off < request.size()) {
        if (cancelled()) return interrupted_error();
#if defined(MSG_NOSIGNAL)
        const auto n = ::send(s.get(), request.data() + off, request.size() - off, MSG_NOSIGNAL);
#else
        const auto n = ::send(s.get(), request.data() + off, request.size() - off, 0);
#endif
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int left = ms_until(deadline_);
            if (left <= 0) return timeout_error("sending the request");
            pollfd fds[2] = {{s.get(), POLLOUT, 0}, {opt_.cancel_fd, POLLIN, 0}};
            (void)::poll(fds, opt_.cancel_fd >= 0 ? 2 : 1, left);
            continue;
        }
        // The channel closed while sending: ssh could not open it remotely.
        break;
    }

    // Receive until the reply is complete, or EOF, or an error.
    std::string buf;
    char tmp[65536];
    bool eof = false;
    while (true) {
        const auto n = ::recv(s.get(), tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            const auto r = parse_reply(buf, false);
            if (r.state == ReplyParse::State::complete) return r.reply;
            if (r.state == ReplyParse::State::error) {
                RemoteError e;
                e.code = RemoteErrorCode::protocol_error;
                e.message = "Malformed response from the remote agent: " + r.error + ".";
                return e;
            }
            continue;
        }
        if (n == 0 || (n < 0 && (errno == ECONNRESET || errno == EPIPE))) {
            eof = true;
            break;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            eof = true;
            break;
        }
        if (cancelled()) return interrupted_error();
        const int left = ms_until(deadline_);
        if (left <= 0) return timeout_error("waiting for the agent's response");
        pollfd fds[2] = {{s.get(), POLLIN, 0}, {opt_.cancel_fd, POLLIN, 0}};
        (void)::poll(fds, opt_.cancel_fd >= 0 ? 2 : 1, left);
    }
    (void)eof;
    if (buf.empty()) {
        // No byte at all: the channel was refused remotely, or ssh died.
        drain_stderr(std::chrono::milliseconds(50));
        if (reap_nonblocking()) return exited_error();
        return classify_channel_eof(stderr_);
    }
    const auto r = parse_reply(buf, true);
    if (r.state == ReplyParse::State::complete) return r.reply;
    RemoteError e;
    e.code = RemoteErrorCode::protocol_error;
    e.message = "Malformed response from the remote agent: " + r.error + ".";
    return e;
}

void SshTunnel::close() {
    if (pid_ > 0 && !reap_nonblocking()) {
        // The group holds ssh and anything it started (ProxyCommand).
        if (::kill(-pid_, SIGTERM) != 0) ::kill(pid_, SIGTERM);
        const auto until = Clock::now() + opt_.term_grace;
        while (!reap_nonblocking() && Clock::now() < until) {
            pollfd p{-1, 0, 0};
            (void)::poll(&p, 0, 2);
        }
        if (!reaped_) {
            if (::kill(-pid_, SIGKILL) != 0) ::kill(pid_, SIGKILL);
            int st = 0;
            pid_t r;
            do r = ::waitpid(pid_, &st, 0);
            while (r < 0 && errno == EINTR);
            reaped_ = true;
            wait_status_ = st;
        }
    }
    err_.reset();
    if (dir_) dir_->remove();
}

SshTunnel::~SshTunnel() { close(); }

// --------------------------------------------------------- SignalGuard ---

SignalGuard::SignalGuard() {
    int p[2];
    if (cloexec_pipe(p)) {
        set_cloexec_nonblock(p[0], true);
        set_cloexec_nonblock(p[1], true);
        g_signal_fd_r = p[0];
        g_signal_fd_w = p[1];
    }
    g_caught = 0;
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (std::size_t i = 0; i < 4; ++i) {
        struct sigaction use = sa;
        if (kGuarded[i] == SIGPIPE) use.sa_handler = SIG_IGN;
        ::sigaction(kGuarded[i], &use, &g_old[i]);
    }
}

SignalGuard::~SignalGuard() {
    for (std::size_t i = 0; i < 4; ++i) ::sigaction(kGuarded[i], &g_old[i], nullptr);
    const int w = g_signal_fd_w.exchange(-1), r = g_signal_fd_r.exchange(-1);
    if (w >= 0) ::close(w);
    if (r >= 0) ::close(r);
}

int SignalGuard::fd() const noexcept { return g_signal_fd_r.load(); }
int SignalGuard::caught() const noexcept { return g_caught.load(); }

void SignalGuard::reraise() {
    const int sig = g_caught.load();
    if (sig == 0) return;
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    ::sigaction(sig, &sa, nullptr);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    ::sigprocmask(SIG_UNBLOCK, &set, nullptr);
    ::raise(sig);
}

}  // namespace osie::remote
