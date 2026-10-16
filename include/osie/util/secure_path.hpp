// SPDX-License-Identifier: Apache-2.0
// Opening files and directories without trusting the path (POSIX).
//
// Used for everything the agent's security depends on: its configuration
// file and the directory of its API socket. The path is walked one
// component at a time: each directory is opened relative to the previous one
// with O_NOFOLLOW and validated on its descriptor, so replacing a component
// after it was checked cannot redirect the walk. Rules:
// - every directory must be owned by root or the calling user (euid), and
//   not writable by group or others unless it is sticky (then others cannot
//   rename or remove entries they do not own);
// - a symbolic link is followed only if root or the calling user owns it,
//   and its target is walked by the same rules;
// - the final object is validated on the descriptor that is returned.
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "osie/core/result.hpp"

namespace osie::secure {

/// Owns a file descriptor; closes it on destruction.
class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    void reset() noexcept;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

struct OpenOptions {
    /// Prefix of error messages, e.g. "agent config".
    std::string_view what{"path"};
    /// Directories only: the final directory must also not be writable by
    /// group or others at all (sticky is not enough), and must be owned by
    /// the calling user or root. For a directory in which a socket is created.
    bool exclusive_final_dir{false};
};

/// Opens a regular file for reading. Refuses non-regular files (a FIFO never
/// blocks), files owned by another user, and files writable by group or
/// others.
Result<Fd> open_trusted_file(const std::string& path, const OpenOptions& opt = {});

/// Opens a directory (O_DIRECTORY) by the same rules.
Result<Fd> open_trusted_dir(const std::string& path, const OpenOptions& opt = {});

/// True if `uid` may control objects the agent trusts: root or the euid.
bool trusted_owner(unsigned uid) noexcept;

}  // namespace osie::secure
