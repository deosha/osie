// SPDX-License-Identifier: Apache-2.0
// One OpenSSH connection to a remote agent (docs/REMOTE.md, "What runs").
//
//   osie ── AF_UNIX ──► <private dir>/fwd.sock ── ssh -L ──► sshd ── AF_UNIX ──► agent socket
//                        (0700 dir, 0600 socket)                  (as the login user)
//
// `ssh` is spawned with a fixed argument vector (posix_spawn on macOS,
// fork+execve on Linux; never a shell) in its own process group, with stdin
// and stdout on /dev/null, stderr on a bounded pipe, and no other
// descriptor. It forwards a Unix socket in a fresh private directory to the
// agent's socket on the remote host. Each request is one connection to that
// socket, speaking the agent's existing HTTP/JSON API. The remote sshd
// session process connects to the agent as the SSH login user, so the
// agent's kernel peer credentials, access tiers, redaction and per-uid
// sessions apply unchanged.
//
// Every step is bounded by one deadline. On every exit path (success,
// error, timeout, cancellation, exceptions) ssh's process group is sent
// SIGTERM, then SIGKILL after a short grace, ssh is reaped, every descriptor
// is closed and the private directory is removed.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "osie/remote/errors.hpp"
#include "osie/remote/http.hpp"
#include "osie/remote/profile.hpp"
#include "osie/util/secure_path.hpp"

namespace osie::remote {

using Clock = std::chrono::steady_clock;

/// A value or a transport failure.
template <class T>
class Outcome {
public:
    Outcome(T v) : v_(std::move(v)) {}             // NOLINT(google-explicit-constructor)
    Outcome(RemoteError e) : v_(std::move(e)) {}   // NOLINT(google-explicit-constructor)
    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }
    T& value() { return std::get<0>(v_); }
    const T& value() const { return std::get<0>(v_); }
    T* operator->() { return &value(); }
    [[nodiscard]] const RemoteError& error() const { return std::get<1>(v_); }

private:
    std::variant<T, RemoteError> v_;
};

/// A fresh private directory for the forwarding socket: created with
/// mkdirat() under a validated parent ($XDG_RUNTIME_DIR, else $TMPDIR, else
/// /tmp; the first that is absolute, trusted by osie::secure rules, made of
/// [A-Za-z0-9/._-] and short enough for sun_path), named osie-remote-<96
/// random bits>, mode 0700, owned by the user, checked on its descriptor.
/// Removed (with whatever is inside) on destruction.
class PrivateDir {
public:
    static Result<std::unique_ptr<PrivateDir>> create();
    ~PrivateDir();
    PrivateDir(const PrivateDir&) = delete;
    PrivateDir& operator=(const PrivateDir&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string socket_path() const { return path_ + "/fwd.sock"; }
    [[nodiscard]] int fd() const noexcept { return dir_.get(); }
    /// Removes the contents and the directory now (idempotent).
    void remove();

private:
    PrivateDir() = default;
    secure::Fd parent_, dir_;
    std::string name_, path_;
};

struct TunnelOptions {
    std::string ssh_path;                       // absolute (locate_ssh())
    std::chrono::milliseconds timeout{std::chrono::seconds(kDefaultTimeoutS)};
    /// Readable means "stop now" (the CLI's signal self-pipe); -1 for none.
    int cancel_fd{-1};
    /// Between SIGTERM and SIGKILL when stopping ssh.
    std::chrono::milliseconds term_grace{500};
};

class SshTunnel {
public:
    /// Spawns ssh and waits until the forwarding socket exists and is a
    /// socket owned by the user, or ssh exits (classified from its status and
    /// stderr), or the deadline passes, or `cancel_fd` becomes readable.
    static Outcome<std::unique_ptr<SshTunnel>> open(const Profile& p, const TunnelOptions& o);

    /// One HTTP exchange on a new connection to the forwarding socket,
    /// within the remaining deadline. The socket's peer must be the ssh
    /// process (kernel credentials: same uid, and its pid where reported).
    Outcome<HttpReply> exchange(const std::string& request);

    /// Stops ssh (SIGTERM to its process group, SIGKILL after the grace),
    /// reaps it, closes descriptors and removes the private directory.
    /// Idempotent; also done by the destructor.
    void close();
    ~SshTunnel();
    SshTunnel(const SshTunnel&) = delete;
    SshTunnel& operator=(const SshTunnel&) = delete;

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] const std::string& private_dir() const;
    /// From spawn to the forwarding socket being ready.
    [[nodiscard]] double ready_ms() const noexcept { return ready_ms_; }
    [[nodiscard]] Clock::time_point deadline() const noexcept { return deadline_; }

private:
    SshTunnel() = default;
    void drain_stderr(std::chrono::milliseconds wait);
    /// Checks whether ssh has exited (non-blocking); records its status.
    bool reap_nonblocking();
    RemoteError exited_error();
    RemoteError timeout_error(const char* stage);
    RemoteError interrupted_error();
    bool cancelled() const;

    TunnelOptions opt_;
    std::unique_ptr<PrivateDir> dir_;
    pid_t pid_{-1};
    bool reaped_{false};
    int wait_status_{0};
    secure::Fd err_;  // read end of ssh's stderr
    std::string stderr_;
    Clock::time_point start_{}, deadline_{};
    double ready_ms_{0};
};

/// Installs handlers for SIGINT, SIGTERM and SIGHUP that record the signal
/// and make fd() readable, ignores SIGPIPE and resets SIGCHLD to its
/// default (an inherited SIG_IGN would lose ssh's status); restores the previous
/// dispositions on destruction. One instance at a time.
class SignalGuard {
public:
    SignalGuard();
    ~SignalGuard();
    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
    [[nodiscard]] int fd() const noexcept;
    /// The first signal received, or 0.
    [[nodiscard]] int caught() const noexcept;
    /// Restores the default action of the caught signal and raises it again,
    /// so the exit status reports it. Returns if no signal was caught.
    void reraise();
};

}  // namespace osie::remote
