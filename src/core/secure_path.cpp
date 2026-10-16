// SPDX-License-Identifier: Apache-2.0
#include "osie/util/secure_path.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>

namespace osie::secure {
namespace {

constexpr int kMaxSymlinks = 32;

Error refused(const OpenOptions& o, const std::string& what, const std::string& why) {
    return Error{Errc::permission_denied, std::string(o.what) + ": " + what + " " + why + "; refusing to use it", 0};
}

Error os_error(const OpenOptions& o, const std::string& what, int err) {
    if (err == ENOENT || err == ENOTDIR) return Error{Errc::not_found, std::string(o.what) + " " + what + " not found", err};
    if (err == EACCES || err == EPERM) return Error{Errc::permission_denied, "cannot read " + std::string(o.what) + " " + what, err};
    return Error{Errc::io_error, std::string(o.what) + " " + what + ": " + std::strerror(err), err};
}

Result<void> check_dir(int fd, const OpenOptions& o, const std::string& what, bool exclusive) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) return os_error(o, what, errno);
    if (!trusted_owner(st.st_uid)) return refused(o, "directory " + what, "is owned by uid " + std::to_string(st.st_uid));
    const bool shared = (st.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    if (shared && (exclusive || (st.st_mode & S_ISVTX) == 0))
        return refused(o, "directory " + what, "is writable by group or others");
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

Result<Fd> open_dir_at(int at, const char* name, const OpenOptions& o, const std::string& what, bool exclusive) {
    Fd d(::openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!d) {
        if (errno == ELOOP) return refused(o, what, "was replaced by a symbolic link while being opened");
        return os_error(o, what, errno);
    }
    if (auto r = check_dir(d.get(), o, what, exclusive); !r) return r.error();
    return d;
}

/// Walks `path`. Returns the descriptor of its final component: opened as a
/// directory if `want_dir`, else as a regular file for reading.
Result<Fd> walk(const std::string& path, bool want_dir, const OpenOptions& o) {
    if (path.empty()) return Error{Errc::not_found, std::string(o.what) + " path is empty", 0};
    std::deque<std::string> parts = split_path(path);
    std::string walked = path[0] == '/' ? "/" : ".";
    const bool exclusive_root = want_dir && o.exclusive_final_dir && parts.empty();
    auto dir = open_dir_at(AT_FDCWD, walked.c_str(), o, walked, exclusive_root);
    if (!dir) return dir.error();
    Fd cur = std::move(dir).value();
    auto join = [](const std::string& a, const std::string& b) { return a == "/" ? "/" + b : a + "/" + b; };
    int links = 0;
    while (!parts.empty()) {
        const std::string name = std::move(parts.front());
        parts.pop_front();
        const std::string what = join(walked, name);
        struct stat lst {};
        if (::fstatat(cur.get(), name.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) return os_error(o, what, errno);
        if (S_ISLNK(lst.st_mode)) {
            if (++links > kMaxSymlinks)
                return Error{Errc::invalid_argument, std::string(o.what) + " " + path + ": too many symbolic links", ELOOP};
            if (!trusted_owner(lst.st_uid))
                return refused(o, "symbolic link " + what, "is owned by uid " + std::to_string(lst.st_uid));
            char buf[4096];
            const auto n = ::readlinkat(cur.get(), name.c_str(), buf, sizeof buf);
            if (n < 0) return os_error(o, what, errno);
            if (static_cast<std::size_t>(n) >= sizeof buf)
                return Error{Errc::invalid_argument, std::string(o.what) + " " + what + ": link target too long", 0};
            const std::string target(buf, static_cast<std::size_t>(n));
            if (target.empty()) return os_error(o, what, ENOENT);
            auto t = split_path(target);
            parts.insert(parts.begin(), t.begin(), t.end());
            if (target[0] == '/') {
                walked = "/";
                auto root = open_dir_at(AT_FDCWD, "/", o, walked, false);
                if (!root) return root.error();
                cur = std::move(root).value();
            }
            continue;
        }
        if (!parts.empty() || want_dir) {
            const bool exclusive = want_dir && parts.empty() && o.exclusive_final_dir;
            auto next = open_dir_at(cur.get(), name.c_str(), o, what, exclusive);
            if (!next) return next.error();
            cur = std::move(next).value();
            walked = what;
            continue;
        }
        // The file. O_NONBLOCK: opening a FIFO must not block; it is rejected below.
        Fd f(::openat(cur.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
        if (!f) {
            if (errno == ELOOP) return refused(o, what, "was replaced by a symbolic link while being opened");
            return os_error(o, what, errno);
        }
        struct stat st {};
        if (::fstat(f.get(), &st) != 0) return os_error(o, what, errno);
        if (!S_ISREG(st.st_mode))
            return Error{Errc::invalid_argument, std::string(o.what) + " " + what + " is not a regular file", 0};
        if (!trusted_owner(st.st_uid)) return refused(o, what, "is owned by uid " + std::to_string(st.st_uid));
        if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return refused(o, what, "is writable by group or others (chmod go-w)");
        return f;
    }
    if (want_dir) return cur;
    return Error{Errc::invalid_argument, std::string(o.what) + " " + path + " is not a regular file", 0};
}

}  // namespace

void Fd::reset() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool trusted_owner(unsigned uid) noexcept { return uid == 0 || uid == ::geteuid(); }

Result<Fd> open_trusted_file(const std::string& path, const OpenOptions& opt) { return walk(path, false, opt); }

Result<Fd> open_trusted_dir(const std::string& path, const OpenOptions& opt) { return walk(path, true, opt); }

}  // namespace osie::secure
