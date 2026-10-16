// SPDX-License-Identifier: Apache-2.0
#include "osie/agent/config.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <set>
#include <sstream>

namespace osie::agent {
namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

Error bad(std::size_t line, const std::string& m) {
    return Error{Errc::invalid_argument, "agent config line " + std::to_string(line) + ": " + m, 0};
}

std::optional<long long> parse_int(const std::string& v) {
    if (v.empty() || v.size() > 12) return std::nullopt;
    long long r = 0;
    for (char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        r = r * 10 + (c - '0');
    }
    return r;
}

std::optional<Millis> parse_duration(const std::string& v) {
    std::size_t i = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
    const auto n = parse_int(v.substr(0, i));
    if (!n) return std::nullopt;
    const std::string unit = v.substr(i);
    long long ms = 0;
    if (unit == "ms") ms = *n;
    else if (unit == "s") ms = *n * 1000;
    else if (unit == "m") ms = *n * 60'000;
    else if (unit == "h") ms = *n * 3'600'000;
    else return std::nullopt;
    return Millis{ms};
}

std::optional<ObservationKind> kind_from(std::string_view s) {
    for (unsigned k = 0; k < static_cast<unsigned>(ObservationKind::count_); ++k)
        if (to_string(static_cast<ObservationKind>(k)) == s) return static_cast<ObservationKind>(k);
    return std::nullopt;
}

std::string dur(Millis m) {
    const auto v = m.count();
    if (v % 3'600'000 == 0 && v) return std::to_string(v / 3'600'000) + "h";
    if (v % 60'000 == 0 && v) return std::to_string(v / 60'000) + "m";
    if (v % 1000 == 0 && v) return std::to_string(v / 1000) + "s";
    return std::to_string(v) + "ms";
}

}  // namespace

std::string_view to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::error: return "error";
        case LogLevel::warn: return "warn";
        case LogLevel::info: return "info";
        case LogLevel::debug: return "debug";
    }
    return "?";
}

Result<AgentConfig> parse_agent_config(std::string_view text) {
    if (text.size() > 64 * 1024) return Error{Errc::invalid_argument, "agent config larger than 64 KiB", 0};
    AgentConfig c;
    std::set<std::string> seen;
    std::istringstream in{std::string(text)};
    std::string raw;
    std::size_t lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        if (lineno > 1000) return bad(lineno, "too many lines");
        const auto hash = raw.find('#');
        const std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) return bad(lineno, "expected 'key = value'");
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) return bad(lineno, "empty key or value");
        if (!seen.insert(key).second) return bad(lineno, "duplicate key '" + key + "'");
        auto duration_in = [&](Millis lo, Millis hi, Millis& out) -> Result<void> {
            const auto d = parse_duration(val);
            if (!d) return bad(lineno, key + ": expected a duration such as 500ms, 5s, 2m or 1h");
            if (*d < lo || *d > hi) return bad(lineno, key + " must be between " + dur(lo) + " and " + dur(hi));
            out = *d;
            return {};
        };
        auto count_in = [&](long long lo, long long hi, std::size_t& out) -> Result<void> {
            const auto n = parse_int(val);
            if (!n) return bad(lineno, key + ": expected a non-negative integer");
            if (*n < lo || *n > hi) return bad(lineno, key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
            out = static_cast<std::size_t>(*n);
            return {};
        };
        Result<void> r;
        if (key == "interval") r = duration_in(Millis{250}, Millis{3'600'000}, c.interval);
        else if (key == "sample_window") r = duration_in(Millis{50}, Millis{5000}, c.sample_window);
        else if (key == "history_capacity") r = count_in(8, 100'000, c.history_capacity);
        else if (key == "history_top_k") r = count_in(1, 64, c.history_top_k);
        else if (key == "cgroup_top_n") r = count_in(0, 64, c.cgroup_top_n);
        else if (key == "failure_threshold") r = count_in(1, 100, c.failure_threshold);
        else if (key == "health_log_interval") r = duration_in(Millis{0}, Millis{86'400'000}, c.health_log_interval);
        else if (key == "log_level") {
            if (val == "error") c.log_level = LogLevel::error;
            else if (val == "warn") c.log_level = LogLevel::warn;
            else if (val == "info") c.log_level = LogLevel::info;
            else if (val == "debug") c.log_level = LogLevel::debug;
            else r = bad(lineno, "log_level must be error, warn, info or debug");
        } else if (key == "observe") {
            c.observe.clear();
            std::istringstream ks(val);
            for (std::string item; std::getline(ks, item, ',');) {
                const auto k = kind_from(trim(item));
                if (!k) return bad(lineno, "unknown observation kind '" + trim(item) + "'");
                if (std::find(c.observe.begin(), c.observe.end(), *k) == c.observe.end()) c.observe.push_back(*k);
            }
            if (c.observe.empty()) r = bad(lineno, "observe must name at least one observation kind");
        } else if (key == "listen" || key == "bind" || key == "port" || key == "persist" || key == "persistence" ||
                   key == "database" || key == "remote" || key == "upload" || key == "endpoint") {
            r = bad(lineno, "'" + key + "' is not supported: this agent keeps all data in memory and has no network "
                            "listener or remote access");
        } else {
            r = bad(lineno, "unknown key '" + key + "'");
        }
        if (!r) return r.error();
    }
    // Cross-field rules.
    if (c.sample_window * 2 > c.interval)
        return Error{Errc::invalid_argument,
                     "agent config: sample_window (" + dur(c.sample_window) + ") must be at most half of interval (" +
                         dur(c.interval) + ")",
                     0};
    return c;
}

namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr int kMaxSymlinks = 32;

/// Closes a descriptor on scope exit.
struct Fd {
    int fd{-1};
    explicit Fd(int f = -1) : fd(f) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd = std::exchange(o.fd, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    void reset() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

Error refused(const std::string& what, const std::string& why) {
    return Error{Errc::permission_denied, "agent config: " + what + " " + why + "; refusing to use it", 0};
}

Error os_error(const std::string& what, int err) {
    if (err == ENOENT || err == ENOTDIR) return Error{Errc::not_found, "agent config " + what + " not found", err};
    if (err == EACCES || err == EPERM) return Error{Errc::permission_denied, "cannot read agent config " + what, err};
    return Error{Errc::io_error, "agent config " + what + ": " + std::strerror(err), err};
}

/// Who may control something the configuration depends on: root or the
/// agent's own user.
bool trusted_owner(uid_t uid) { return uid == 0 || uid == ::geteuid(); }

/// A directory on the path must not let anyone else replace its entries:
/// owned by root or us, and not writable by group or others unless sticky
/// (then others cannot rename or remove entries they do not own).
Result<void> check_dir(int fd, const std::string& what) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) return os_error(what, errno);
    if (!trusted_owner(st.st_uid)) return refused("directory " + what, "is owned by uid " + std::to_string(st.st_uid));
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (st.st_mode & S_ISVTX) == 0)
        return refused("directory " + what, "is writable by group or others");
    return {};
}

std::deque<std::string> split_path(const std::string& p) {
    std::deque<std::string> out;
    std::size_t i = 0;
    while (i <= p.size()) {
        const std::size_t j = std::min(p.find('/', i), p.size());
        std::string c = p.substr(i, j - i);
        if (!c.empty() && c != ".") out.push_back(std::move(c));
        i = j + 1;
    }
    return out;
}

Result<Fd> open_dir_at(int at, const char* name, const std::string& what) {
    Fd d(::openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (d.fd < 0) {
        if (errno == ELOOP) return refused(what, "was replaced by a symbolic link while being opened");
        return os_error(what, errno);
    }
    if (auto r = check_dir(d.fd, what); !r) return r.error();
    return d;
}

/// Opens the configuration file without trusting the path: every directory
/// is opened relative to the previous one with O_NOFOLLOW and validated on
/// its descriptor; symbolic links are followed only if root or the agent's
/// user owns them, and their targets are walked the same way. The file
/// itself is validated with fstat() on the descriptor that is then read, so
/// replacing any component after a check cannot substitute another file.
Result<Fd> open_config(const std::string& path) {
    if (path.empty()) return Error{Errc::not_found, "agent config path is empty", 0};
    std::deque<std::string> parts = split_path(path);
    std::string walked = path[0] == '/' ? "/" : ".";
    auto dir = open_dir_at(AT_FDCWD, walked.c_str(), walked);
    if (!dir) return dir.error();
    Fd cur = std::move(dir).value();
    auto join = [](const std::string& a, const std::string& b) { return a == "/" ? "/" + b : a + "/" + b; };
    int links = 0;
    while (!parts.empty()) {
        const std::string name = std::move(parts.front());
        parts.pop_front();
        const std::string what = join(walked, name);
        struct stat lst {};
        if (::fstatat(cur.fd, name.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) return os_error(what, errno);
        if (S_ISLNK(lst.st_mode)) {
            if (++links > kMaxSymlinks) return Error{Errc::invalid_argument, "agent config " + path + ": too many symbolic links", ELOOP};
            if (!trusted_owner(lst.st_uid)) return refused("symbolic link " + what, "is owned by uid " + std::to_string(lst.st_uid));
            char buf[4096];
            const auto n = ::readlinkat(cur.fd, name.c_str(), buf, sizeof buf);
            if (n < 0) return os_error(what, errno);
            if (static_cast<std::size_t>(n) >= sizeof buf) return Error{Errc::invalid_argument, "agent config " + what + ": link target too long", 0};
            const std::string target(buf, static_cast<std::size_t>(n));
            if (target.empty()) return os_error(what, ENOENT);
            auto t = split_path(target);
            parts.insert(parts.begin(), t.begin(), t.end());
            if (target[0] == '/') {
                walked = "/";
                auto root = open_dir_at(AT_FDCWD, "/", walked);
                if (!root) return root.error();
                cur = std::move(root).value();
            }
            continue;
        }
        if (!parts.empty()) {  // an intermediate directory
            auto next = open_dir_at(cur.fd, name.c_str(), what);
            if (!next) return next.error();
            cur = std::move(next).value();
            walked = what;
            continue;
        }
        // The file. O_NONBLOCK: opening a FIFO must not block; it is rejected below.
        Fd f(::openat(cur.fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
        if (f.fd < 0) {
            if (errno == ELOOP) return refused(what, "was replaced by a symbolic link while being opened");
            return os_error(what, errno);
        }
        struct stat st {};
        if (::fstat(f.fd, &st) != 0) return os_error(what, errno);
        if (!S_ISREG(st.st_mode)) return Error{Errc::invalid_argument, "agent config " + what + " is not a regular file", 0};
        if (!trusted_owner(st.st_uid)) return refused(what, "is owned by uid " + std::to_string(st.st_uid));
        if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return refused(what, "is writable by group or others (chmod go-w)");
        return f;
    }
    return Error{Errc::invalid_argument, "agent config " + path + " is not a regular file", 0};
}

}  // namespace

Result<AgentConfig> load_agent_config(const std::string& path) {
    auto f = open_config(path);
    if (!f) return f.error();
    // The size limit is enforced on the bytes read (st_size can change, or lie).
    std::string text;
    char buf[8192];
    while (true) {
        const auto n = ::read(f->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return os_error(path, errno);
        }
        if (n == 0) break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() > kMaxConfigBytes) return Error{Errc::invalid_argument, "agent config " + path + " is larger than 64 KiB", 0};
    }
    return parse_agent_config(text);
}

std::string to_text(const AgentConfig& c) {
    std::string o;
    o += "interval = " + dur(c.interval) + "\n";
    o += "sample_window = " + dur(c.sample_window) + "\n";
    o += "history_capacity = " + std::to_string(c.history_capacity) + "\n";
    o += "history_top_k = " + std::to_string(c.history_top_k) + "\n";
    o += "cgroup_top_n = " + std::to_string(c.cgroup_top_n) + "\n";
    o += "failure_threshold = " + std::to_string(c.failure_threshold) + "\n";
    o += "health_log_interval = " + dur(c.health_log_interval) + "\n";
    o += "log_level = " + std::string(to_string(c.log_level)) + "\n";
    o += "observe = ";
    for (std::size_t i = 0; i < c.observe.size(); ++i) o += (i ? "," : "") + std::string(to_string(c.observe[i]));
    return o + "\n";
}

}  // namespace osie::agent
