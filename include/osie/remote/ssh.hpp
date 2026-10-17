// SPDX-License-Identifier: Apache-2.0
// The OpenSSH command line of `osie remote` (docs/REMOTE.md, "What runs").
//
// The argument vector is fixed: options OSIE forces for security, then the
// profile's validated fields, then "--" and the destination. No user-supplied
// "-o" option exists, no shell is involved, and the question never appears
// in it. Command-line options take precedence over ssh_config, so a user's
// configuration cannot weaken the forced ones.
#pragma once

#include <string>
#include <vector>

#include "osie/core/result.hpp"
#include "osie/remote/profile.hpp"

namespace osie::remote {

/// The options forced on every connection, in order (each is passed as
/// "-o" followed by the value).
const std::vector<std::string>& forced_ssh_options();

/// Builds argv (argv[0] = `ssh_path`) for a local Unix-socket forward from
/// `local_socket` to the profile's remote agent socket. `connect_timeout_s`
/// bounds TCP connection and SSH handshake (ConnectTimeout). The profile is
/// re-validated; an invalid one is an error, never a partial command line.
Result<std::vector<std::string>> build_ssh_argv(const std::string& ssh_path, const Profile& p,
                                                const std::string& local_socket, int connect_timeout_s);

/// Locates the OpenSSH client. `$OSIE_SSH`, if set, must be an absolute path
/// to an executable regular file (used by tests; see docs/REMOTE.md). Else
/// the first executable `ssh` in the absolute entries of $PATH (relative
/// entries are skipped), else /usr/bin/ssh. In a set-id context neither
/// variable is consulted. Errc::not_found if there is none.
Result<std::string> locate_ssh();

}  // namespace osie::remote
