// SPDX-License-Identifier: Apache-2.0
// Remote profiles: named SSH destinations of osie agents (docs/REMOTE.md).
//
// Every field is validated against a strict grammar before it is stored and
// again when it is loaded, because each one ends up in the argument vector
// of `ssh`: no leading '-', no whitespace, control characters, quotes, shell
// metacharacters, '%' tokens or '$' variables, and absolute paths where
// paths are expected. Profiles hold no secrets: keys and agents stay
// OpenSSH's business.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/core/result.hpp"

namespace osie::remote {

inline constexpr int kDefaultTimeoutS = 15;
inline constexpr int kMaxTimeoutS = 120;
inline constexpr int kMinTimeoutS = 1;
inline constexpr std::string_view kDefaultRemoteSocket = "/run/osie/agent.sock";
inline constexpr std::size_t kMaxProfiles = 256;
inline constexpr std::size_t kMaxProfileFileBytes = 64 * 1024;

struct Profile {
    std::string name;         // [A-Za-z0-9._-]{1,64}, not starting with '-' or '.'
    std::string destination;  // [user@]host, host a hostname, IPv4 or [IPv6]
    std::optional<int> port;  // 1-65535; absent: OpenSSH's choice (ssh_config, else 22)
    std::string socket{kDefaultRemoteSocket};  // the agent's socket on the remote host
    std::string identity;     // optional absolute path (IdentityFile, IdentitiesOnly=yes)
    std::string known_hosts;  // optional absolute path (UserKnownHostsFile)
    int timeout_s{kDefaultTimeoutS};

    friend bool operator==(const Profile&, const Profile&) = default;
};

// Field validators. Each returns an error message, or nothing if valid.
std::optional<std::string> check_name(std::string_view s);
std::optional<std::string> check_destination(std::string_view s);
std::optional<std::string> check_port(long v);
std::optional<std::string> check_timeout(long v);
/// A remote agent socket path: absolute, at most 100 bytes (the agent's own
/// limit), characters [A-Za-z0-9/._-], no "." or ".." components.
std::optional<std::string> check_socket_path(std::string_view s);
/// A local file path given to OpenSSH (identity, known_hosts): absolute, at
/// most 1024 bytes, characters [A-Za-z0-9/._+@-], no "." or ".." components.
std::optional<std::string> check_file_path(std::string_view s);
std::optional<std::string> check_profile(const Profile& p);

/// Parses a decimal integer made only of digits (no sign, no spaces), at
/// most 9 digits.
std::optional<long> parse_decimal(std::string_view s);

/// The host part of a destination as OpenSSH wants it on its command line:
/// "user@[::1]" becomes "user@::1" (OpenSSH does not strip brackets).
std::string ssh_destination(std::string_view destination);

/// Text format (strict; any deviation is an error that names the line):
///
///   # comment
///   [name]
///   destination = user@host
///   port = 22
///   socket = /run/osie/agent.sock
///   identity = /home/u/.ssh/id_ed25519
///   known_hosts = /home/u/.ssh/known_hosts
///   timeout = 15
///
/// Unknown keys, duplicate keys or sections, keys outside a section, and
/// sections without a destination are refused.
Result<std::vector<Profile>> parse_profiles(std::string_view text);
std::string serialize_profiles(const std::vector<Profile>& profiles);

/// The profile file: $XDG_CONFIG_HOME/osie/remotes.conf (XDG_CONFIG_HOME
/// only if absolute), else $HOME/.config/osie/remotes.conf.
class ProfileStore {
public:
    /// `dir` is the directory that holds remotes.conf.
    explicit ProfileStore(std::string dir) : dir_(std::move(dir)) {}
    /// The default location for the calling user, or an error if neither
    /// XDG_CONFIG_HOME nor HOME gives an absolute path.
    static Result<ProfileStore> for_user();

    [[nodiscard]] const std::string& dir() const noexcept { return dir_; }
    [[nodiscard]] std::string file() const { return dir_ + "/remotes.conf"; }

    /// Reads all profiles. A missing file or directory is an empty list. The
    /// directory must be owned by the user with mode 0700 (no group or other
    /// bits), every directory above it trusted (osie::secure rules), and the
    /// file owned by the user, a regular file, mode 0600 or stricter, at most
    /// kMaxProfileFileBytes.
    Result<std::vector<Profile>> load() const;

    /// Replaces the file atomically, under an exclusive lock on the
    /// directory: a temporary file (O_EXCL, 0600) is written, fsynced and
    /// renamed over remotes.conf, then the directory is fsynced. Creates the
    /// directory (0700) if missing. `edit` receives the current profiles and
    /// changes them; an error it returns aborts without writing.
    Result<void> update(const std::function<Result<void>(std::vector<Profile>&)>& edit) const;

private:
    std::string dir_;
};

}  // namespace osie::remote
