// SPDX-License-Identifier: Apache-2.0
// Who is on the other end of a Unix domain socket, and what they may see.
//
// Identity comes from the kernel at connect time (Linux: SO_PEERCRED and
// SO_PEERGROUPS; macOS: LOCAL_PEERCRED), never from anything the client
// sends. Authorisation is decided from that identity alone, before the
// request is parsed, so rewording a question cannot change it.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osie/state/state.hpp"

namespace osie::api {

struct PeerCredentials {
    bool known{false};  // false: the kernel did not report credentials
    uid_t uid{0};
    gid_t gid{0};
    std::optional<pid_t> pid;  // Linux only
    std::vector<gid_t> groups;  // supplementary groups, when the kernel reports them
};

/// Credentials of the peer connected on `fd` (a connected AF_UNIX socket).
PeerCredentials peer_of(int fd);

/// What a peer may use, from least to most.
enum class Access : std::uint8_t {
    none,         // credentials unknown: nothing
    health,       // any local peer that can open the socket: liveness and readiness
    diagnostics,  // members of api_group: every endpoint, other users' executable paths redacted
    full,         // root and the agent's own user: every endpoint, nothing redacted
};
std::string_view to_string(Access a) noexcept;

struct AccessPolicy {
    uid_t agent_uid{0};
    std::optional<gid_t> group;  // api_group, resolved
};

Access authorize(const PeerCredentials& p, const AccessPolicy& policy) noexcept;

/// How much of other users' processes a non-full peer may see.
enum class ProcVisibility : std::uint8_t {
    all,       // /proc shows every process to every user (the Linux default, and macOS)
    own_only,  // /proc is mounted with hidepid: peers see only their own processes
};

/// Linux: reads /proc's mount options (hidepid / subset). macOS: all.
ProcVisibility detect_proc_visibility();
/// The same decision from mountinfo text (one mount per line): own_only when
/// the procfs mounted at /proc has hidepid set to anything but 0/off.
ProcVisibility proc_visibility_from_mountinfo(std::string_view mountinfo);

/// Removes from `s` what `peer_uid` could not read from the kernel itself:
/// the executable path of other users' processes (as /proc/<pid>/exe), and,
/// under hidepid, other users' processes altogether. Their CPU and memory
/// still count towards host totals, which every user can read.
void redact(SystemState& s, uid_t peer_uid, ProcVisibility v);

}  // namespace osie::api
