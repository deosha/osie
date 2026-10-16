// SPDX-License-Identifier: Apache-2.0
#include "osie/api/server.hpp"

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace osie::api {
namespace {

using Clock = std::chrono::steady_clock;

bool set_nonblock_cloexec(int fd) {
    const int fl = ::fcntl(fd, F_GETFL);
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return false;
    return ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

void no_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

ssize_t send_some(int fd, const char* p, std::size_t n) {
#if defined(MSG_NOSIGNAL)
    return ::send(fd, p, n, MSG_NOSIGNAL);
#else
    return ::send(fd, p, n, 0);
#endif
}

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 60'000));
}

/// True if the peer has closed the connection.
bool peer_gone(int fd) {
    pollfd p{fd, 0, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

ApiError err(int status, ErrorCode code, std::string reason, std::string message, bool retryable = false,
             std::optional<int> retry_after = std::nullopt) {
    ApiError e;
    e.status = status;
    e.code = code;
    e.reason = std::move(reason);
    e.message = std::move(message);
    e.retryable = retryable;
    e.retry_after_s = retry_after;
    return e;
}

Error setup_error(const std::string& m, int e = 0) {
    return Error{Errc::invalid_argument, "api socket: " + m + (e ? std::string(": ") + std::strerror(e) : ""), e};
}

}  // namespace

ApiServer::ApiServer(std::shared_ptr<agent::AgentRuntime> rt, const agent::AgentConfig& cfg, Logger log)
    : rt_(std::move(rt)), cfg_(cfg), log_(std::move(log)), path_(cfg.api_socket) {
    const auto slash = path_.rfind('/');
    dir_ = slash == 0 ? "/" : path_.substr(0, slash);
    name_ = path_.substr(slash + 1);
    ApiSettings s = settings_from(cfg_);
    s.visibility = detect_proc_visibility();
    api_ = std::make_unique<DiagnosticApi>(rt_, s);
    policy_.agent_uid = ::geteuid();
}

ApiServer::~ApiServer() { stop(); }

Result<void> ApiServer::open_socket() {
    if (path_.empty() || path_[0] != '/' || name_.empty()) return setup_error("api_socket must be an absolute path");
    sockaddr_un sa{};
    if (path_.size() >= sizeof sa.sun_path) return setup_error("path too long for a Unix domain socket");
    auto dir = secure::open_trusted_dir(dir_, secure::OpenOptions{"api socket directory", true});
    if (!dir) return dir.error();
    dir_fd_ = std::move(dir).value();
    // One agent per directory. Held until the server stops.
    if (::flock(dir_fd_.get(), LOCK_EX | LOCK_NB) != 0)
        return setup_error(errno == EWOULDBLOCK ? "another osie-agent is serving from " + dir_ : "cannot lock " + dir_,
                           errno == EWOULDBLOCK ? 0 : errno);
    if (!cfg_.api_group.empty()) {
        group g{};
        group* res = nullptr;
        std::vector<char> buf(16384);
        if (::getgrnam_r(cfg_.api_group.c_str(), &g, buf.data(), buf.size(), &res) != 0 || res == nullptr)
            return setup_error("api_group '" + cfg_.api_group + "' does not exist");
        policy_.group = g.gr_gid;
    }
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, path_.c_str(), path_.size());
    const auto sa_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_.size() + 1);

    // An existing entry: removed only if it is our own stale socket.
    struct stat st {};
    if (::fstatat(dir_fd_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid())
            return setup_error("refusing to replace " + path_ + ": it is not a socket owned by the agent's user");
        secure::Fd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!probe) return setup_error("socket", errno);
        if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&sa), sa_len) == 0)
            return setup_error("another process is serving on " + path_);
        if (errno != ECONNREFUSED) return setup_error("cannot check the existing socket " + path_, errno);
        if (::unlinkat(dir_fd_.get(), name_.c_str(), 0) != 0) return setup_error("cannot remove stale socket", errno);
        if (log_) log_(agent::LogLevel::info, "api: removed a stale socket at " + path_);
    } else if (errno != ENOENT) {
        return setup_error("cannot inspect " + path_, errno);
    }

    listen_fd_ = secure::Fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!listen_fd_ || !set_nonblock_cloexec(listen_fd_.get())) return setup_error("socket", errno);
    if (::bind(listen_fd_.get(), reinterpret_cast<const sockaddr*>(&sa), sa_len) != 0) return setup_error("bind", errno);
    socket_created_ = true;
    if (::fstatat(dir_fd_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISSOCK(st.st_mode))
        return setup_error("the socket disappeared after bind");
    sock_dev_ = static_cast<std::uint64_t>(st.st_dev);
    sock_ino_ = static_cast<std::uint64_t>(st.st_ino);
    // Permissions before listen(): until then every connection is refused.
    // A new socket already has the process's group (systemd: Group=osie), so no
    // chown is needed in the service, whose system-call filter forbids it.
    if (policy_.group && st.st_gid != *policy_.group) {
        if (::fchownat(dir_fd_.get(), name_.c_str(), static_cast<uid_t>(-1), *policy_.group, AT_SYMLINK_NOFOLLOW) != 0)
            return setup_error("cannot give the socket to group '" + cfg_.api_group +
                                   "' (the agent's user must be a member; under systemd the unit's Group=)",
                               errno);
    }
    if (::fchmodat(dir_fd_.get(), name_.c_str(), policy_.group ? 0660 : 0600, 0) != 0) return setup_error("chmod", errno);
    // Admission control happens after accept() (queue, rate limit, refusals with
    // a reason), so the kernel backlog only absorbs bursts. The kernel caps it.
    if (::listen(listen_fd_.get(), std::min(SOMAXCONN, 256)) != 0) return setup_error("listen", errno);
    return {};
}

void ApiServer::close_socket() {
    listen_fd_.reset();
    if (socket_created_ && dir_fd_) {
        // Only the socket this server created: never a replacement.
        struct stat st {};
        if (::fstatat(dir_fd_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(st.st_mode) &&
            static_cast<std::uint64_t>(st.st_dev) == sock_dev_ && static_cast<std::uint64_t>(st.st_ino) == sock_ino_)
            (void)::unlinkat(dir_fd_.get(), name_.c_str(), 0);
        socket_created_ = false;
    }
    dir_fd_.reset();  // releases the flock
}

Result<void> ApiServer::start() {
    std::lock_guard rl(run_mu_);
    if (started_) return setup_error("already started");
    if (auto r = open_socket(); !r) {
        close_socket();
        return r;
    }
    int p[2];
    if (::pipe(p) != 0) {
        close_socket();
        return setup_error("pipe", errno);
    }
    wake_r_ = p[0];
    wake_w_ = p[1];
    (void)set_nonblock_cloexec(wake_r_);
    (void)set_nonblock_cloexec(wake_w_);
    stopping_ = false;
    started_ = true;
    for (std::size_t i = 0; i < cfg_.api_max_clients; ++i) workers_.emplace_back([this] { worker_loop(); });
    acceptor_ = std::thread([this] { accept_loop(); });
    if (log_)
        log_(agent::LogLevel::info, "api: serving on " + path_ + " (mode " + (policy_.group ? "0660, group " + cfg_.api_group : std::string("0600")) +
                                        "; " + std::to_string(cfg_.api_max_clients) + " workers)");
    return {};
}

void ApiServer::stop() {
    std::lock_guard rl(run_mu_);
    if (!started_) return;
    stopping_ = true;
    api_->set_stopping();
    cancel_.cancel();
    const char x = 1;
    (void)!::write(wake_w_, &x, 1);  // level-triggered: wakes every poll on wake_r_
    q_cv_.notify_all();
    if (acceptor_.joinable()) acceptor_.join();  // no new connections after this
    close_socket();
    // Connections still waiting are answered, not dropped.
    std::deque<Conn> left;
    {
        std::lock_guard lk(q_mu_);
        left.swap(queue_);
    }
    for (auto& c : left) {
        quick_reply(c.fd, DiagnosticApi::error_response(err(503, ErrorCode::busy, "stopping", "The agent is stopping.", true, 1)));
        std::lock_guard sl(stats_mu_);
        ++stats_.rejected_stopping;
    }
    q_cv_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
    ::close(wake_r_);
    ::close(wake_w_);
    wake_r_ = wake_w_ = -1;
    started_ = false;
}

ServerStats ApiServer::stats() const {
    std::lock_guard sl(stats_mu_);
    return stats_;
}

void ApiServer::note_status(int status) {
    std::lock_guard sl(stats_mu_);
    if (status < 300) ++stats_.responses_2xx;
    else if (status < 500) ++stats_.responses_4xx;
    else ++stats_.responses_5xx;
}

void ApiServer::log_denial(const std::string& m) {
    if (!log_) return;
    // At most 10 lines a second, so a flood of refused clients cannot flood the journal.
    std::uint64_t suppressed = 0;
    {
        std::lock_guard lk(log_mu_);
        const auto now = Clock::now();
        if (now - log_window_ >= std::chrono::seconds(1)) {
            suppressed = log_suppressed_;
            log_suppressed_ = 0;
            log_window_ = now;
            log_count_ = 0;
        }
        if (++log_count_ > 10) {
            ++log_suppressed_;
            return;
        }
    }
    if (suppressed) log_(agent::LogLevel::warn, "api: " + std::to_string(suppressed) + " further refusals not logged");
    log_(agent::LogLevel::warn, "api: " + m);
}

bool ApiServer::admit_rate(uid_t uid) {
    std::lock_guard lk(rate_mu_);
    const auto now = Clock::now();
    const double rate = static_cast<double>(cfg_.api_rate_limit);
    const double cap = 2.0 * rate;
    auto it = buckets_.find(uid);
    if (it == buckets_.end()) {
        if (buckets_.size() >= 4096) buckets_.erase(buckets_.begin());  // bounded: one entry per client uid
        it = buckets_.emplace(uid, Bucket{cap, now}).first;
    }
    auto& b = it->second;
    b.tokens = std::min(cap, b.tokens + rate * std::chrono::duration<double>(now - b.at).count());
    b.at = now;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

void ApiServer::quick_reply(int fd, const HttpResponse& r) {
    // Best effort, never blocking: the client may not be reading.
    const std::string out = serialize(r);
    (void)send_some(fd, out.data(), out.size());
    note_status(r.status);
    ::close(fd);
}

void ApiServer::accept_loop() {
    // Refused connections are answered at once, then half-closed and drained
    // for a moment, so a client still sending its request reads the refusal
    // instead of a broken pipe. Bounded: 256 at a time, 500 ms and 64 KiB each.
    struct Draining {
        int fd;
        Clock::time_point until;
        std::size_t bytes;
    };
    std::vector<Draining> draining;
    auto refuse = [&](int fd, const HttpResponse& r) {
        const std::string out = serialize(r);
        (void)send_some(fd, out.data(), out.size());
        note_status(r.status);
        (void)::shutdown(fd, SHUT_WR);
        if (draining.size() >= 256) {
            ::close(fd);
            return;
        }
        draining.push_back({fd, Clock::now() + std::chrono::milliseconds(500), 0});
    };
    std::vector<pollfd> fds;
    while (!stopping_) {
        fds.assign({{listen_fd_.get(), POLLIN, 0}, {wake_r_, POLLIN, 0}});
        int timeout = -1;
        for (const auto& d : draining) {
            fds.push_back({d.fd, POLLIN, 0});
            const int left = ms_left(d.until);
            timeout = timeout < 0 ? left : std::min(timeout, left);
        }
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stopping_ || (fds[1].revents & POLLIN)) break;
        // Drain refused connections; close those finished, full or expired.
        const auto now = Clock::now();
        for (std::size_t i = 0; i < draining.size();) {
            auto& d = draining[i];
            bool done = now >= d.until;
            if (!done && (fds[2 + i].revents & (POLLIN | POLLHUP | POLLERR))) {
                char tmp[4096];
                while (true) {
                    const auto n = ::recv(d.fd, tmp, sizeof tmp, 0);
                    if (n > 0) {
                        d.bytes += static_cast<std::size_t>(n);
                        if (d.bytes > 64 * 1024) {
                            done = true;
                            break;
                        }
                        continue;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
                    done = true;  // EOF or error
                    break;
                }
            }
            if (done) {
                ::close(d.fd);
                fds.erase(fds.begin() + static_cast<std::ptrdiff_t>(2 + i));
                draining.erase(draining.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        if (!(fds[0].revents & POLLIN)) continue;
        while (!stopping_) {
            const int fd = ::accept(listen_fd_.get(), nullptr, nullptr);
            if (fd < 0) {
                if (errno == EMFILE || errno == ENFILE) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                break;  // EAGAIN: drained; other errors: retried on the next poll
            }
            if (!set_nonblock_cloexec(fd)) {
                ::close(fd);
                continue;
            }
            no_sigpipe(fd);
            Conn c;
            c.fd = fd;
            c.accepted = Clock::now();
            c.peer = peer_of(fd);
            c.access = authorize(c.peer, policy_);
            {
                std::lock_guard sl(stats_mu_);
                ++stats_.accepted;
            }
            if (c.access == Access::none) {
                {
                    std::lock_guard sl(stats_mu_);
                    ++stats_.rejected_unauthorized;
                }
                log_denial("refused a connection whose peer credentials are unknown");
                refuse(fd, DiagnosticApi::error_response(
                               err(401, ErrorCode::unauthorized, "unknown_peer", "The peer's identity could not be determined.")));
                continue;
            }
            if (!admit_rate(c.peer.uid)) {
                {
                    std::lock_guard sl(stats_mu_);
                    ++stats_.rejected_rate_limited;
                }
                log_denial("rate limit: uid " + std::to_string(c.peer.uid));
                refuse(fd, DiagnosticApi::error_response(err(429, ErrorCode::rate_limited, "rate_limited",
                                                              "Too many requests from this user; retry shortly.", true, 1)));
                continue;
            }
            bool queued = false;
            {
                std::lock_guard lk(q_mu_);
                if (queue_.size() < cfg_.api_max_clients) {
                    queue_.push_back(c);
                    queued = true;
                    std::lock_guard sl(stats_mu_);
                    stats_.queued = queue_.size();
                    stats_.max_queued = std::max(stats_.max_queued, stats_.queued);
                }
            }
            if (queued) {
                q_cv_.notify_one();
            } else {
                {
                    std::lock_guard sl(stats_mu_);
                    ++stats_.rejected_busy;
                }
                refuse(fd, DiagnosticApi::error_response(
                               err(503, ErrorCode::busy, "busy", "All request slots are in use; retry shortly.", true, 1)));
            }
        }
    }
    for (const auto& d : draining) ::close(d.fd);
}

void ApiServer::worker_loop() {
    while (true) {
        Conn c;
        {
            std::unique_lock lk(q_mu_);
            q_cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping, and nothing left to answer
            c = queue_.front();
            queue_.pop_front();
            std::lock_guard sl(stats_mu_);
            stats_.queued = queue_.size();
        }
        if (stopping_) {
            quick_reply(c.fd, DiagnosticApi::error_response(err(503, ErrorCode::busy, "stopping", "The agent is stopping.", true, 1)));
            std::lock_guard sl(stats_mu_);
            ++stats_.rejected_stopping;
            continue;
        }
        {
            std::lock_guard sl(stats_mu_);
            ++stats_.in_flight;
            stats_.max_in_flight = std::max(stats_.max_in_flight, stats_.in_flight);
        }
        serve(c);
        std::lock_guard sl(stats_mu_);
        --stats_.in_flight;
        ++stats_.handled;
    }
}

void ApiServer::serve(Conn c) {
    const auto t0 = Clock::now();
    const auto deadline = c.accepted + cfg_.api_request_timeout;
    secure::Fd fd(c.fd);
    HeadLimits lim;
    lim.max_head_bytes = std::min<std::size_t>(8 * 1024, cfg_.api_max_request_bytes);
    lim.max_body_bytes = cfg_.api_max_request_bytes;
    std::string buf;
    buf.reserve(1024);
    HeadParse head;
    std::string rid;

    auto write_all = [&](const std::string& out) {
        const auto wd = Clock::now() + std::min<Clock::duration>(cfg_.api_request_timeout, std::chrono::seconds(5));
        std::size_t off = 0;
        while (off < out.size()) {
            const auto n = send_some(fd.get(), out.data() + off, out.size() - off);
            if (n > 0) {
                off += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd p{fd.get(), POLLOUT, 0};
                const int left = ms_left(wd);
                if (left == 0 || ::poll(&p, 1, left) <= 0) return false;
                continue;
            }
            return false;
        }
        return true;
    };
    auto respond = [&](const HttpResponse& r) {
        if (!write_all(serialize(r))) {
            std::lock_guard sl(stats_mu_);
            ++stats_.client_gone;
        }
        note_status(r.status);
        if (log_ && cfg_.log_level == agent::LogLevel::debug)
            log_(agent::LogLevel::debug, "api: uid " + std::to_string(c.peer.uid) + " " + head.request.method + " " +
                                             head.request.path + " -> " + std::to_string(r.status) + " (" +
                                             std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - t0).count()) + " ms)");
    };
    auto fail = [&](ApiError e) { respond(DiagnosticApi::error_response(e, rid)); };
    // Reads until `want` bytes are buffered (or the head is complete when want == 0).
    enum class Read { ok, timeout, closed, stopping };
    auto read_more = [&]() -> Read {
        pollfd p[2] = {{fd.get(), POLLIN, 0}, {wake_r_, POLLIN, 0}};
        const int left = ms_left(deadline);
        if (left == 0) return Read::timeout;
        const int rc = ::poll(p, 2, left);
        if (rc == 0) return Read::timeout;
        if (rc < 0) return errno == EINTR ? Read::ok : Read::closed;
        if (p[1].revents & POLLIN) return Read::stopping;
        char tmp[4096];
        const auto n = ::recv(fd.get(), tmp, sizeof tmp, 0);
        if (n == 0) return Read::closed;
        if (n < 0) return (errno == EAGAIN || errno == EINTR) ? Read::ok : Read::closed;
        buf.append(tmp, static_cast<std::size_t>(n));
        return Read::ok;
    };
    auto on_read_failure = [&](Read r) {
        std::lock_guard sl(stats_mu_);
        if (r == Read::timeout) ++stats_.timeouts;
        if (r == Read::closed) ++stats_.client_gone;
        if (r == Read::stopping) ++stats_.rejected_stopping;
    };

    while (true) {
        head = parse_head(buf, lim);
        if (head.state != HeadParse::State::incomplete) break;
        const Read r = read_more();
        if (r == Read::ok) continue;
        on_read_failure(r);
        if (r == Read::timeout) fail(err(408, ErrorCode::invalid_request, "request_timeout", "The request was not received in time."));
        if (r == Read::stopping) fail(err(503, ErrorCode::busy, "stopping", "The agent is stopping.", true, 1));
        return;
    }
    if (head.state == HeadParse::State::error) return fail(head.error);
    rid = head.request.request_id;
    HttpRequest& req = head.request;
    const std::size_t body_len = req.content_length.value_or(0);
    if (req.head_bytes + body_len > cfg_.api_max_request_bytes)
        return fail(err(413, ErrorCode::invalid_request, "request_too_large", "The request is too large."));
    if (req.expect_continue && buf.size() < req.head_bytes + body_len) {
        static const std::string kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!write_all(kContinue)) return;
    }
    while (buf.size() < req.head_bytes + body_len) {
        const Read r = read_more();
        if (r == Read::ok) continue;
        on_read_failure(r);
        if (r == Read::timeout) fail(err(408, ErrorCode::invalid_request, "request_timeout", "The request was not received in time."));
        if (r == Read::stopping) fail(err(503, ErrorCode::busy, "stopping", "The agent is stopping.", true, 1));
        return;
    }
    if (buf.size() > req.head_bytes + body_len)
        return fail(err(400, ErrorCode::invalid_request, "trailing_data",
                        "Data after the request body (pipelining is not supported)."));
    req.body = buf.substr(req.head_bytes, body_len);
    // A client that has already gone gets no work done on its behalf.
    if (peer_gone(fd.get())) {
        std::lock_guard sl(stats_mu_);
        ++stats_.client_gone;
        return;
    }
    HttpResponse resp;
    try {
        resp = api_->handle(req, c.peer, c.access, cancel_.token());
    } catch (const std::exception&) {
        resp = DiagnosticApi::error_response(err(500, ErrorCode::internal_error, "internal_error",
                                                 "The request could not be answered."), rid);
    }
    if (peer_gone(fd.get())) {
        std::lock_guard sl(stats_mu_);
        ++stats_.client_gone;
        return;
    }
    respond(resp);
}

}  // namespace osie::api
