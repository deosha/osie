// SPDX-License-Identifier: Apache-2.0
#include "osie/api/peer.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <sstream>

#if defined(__linux__) && !defined(SO_PEERGROUPS)
#define SO_PEERGROUPS 59
#endif

namespace osie::api {

PeerCredentials peer_of(int fd) {
    PeerCredentials p;
#if defined(__linux__)
    ucred uc{};
    socklen_t len = sizeof uc;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) != 0 || len != sizeof uc) return p;
    p.known = true;
    p.uid = uc.uid;
    p.gid = uc.gid;
    if (uc.pid > 0) p.pid = uc.pid;
    // Supplementary groups at connect time (Linux 4.13+; older kernels: primary group only).
    std::vector<gid_t> g(64);
    auto glen = static_cast<socklen_t>(g.size() * sizeof(gid_t));
    int rc = ::getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, g.data(), &glen);
    if (rc != 0 && errno == ERANGE && glen <= 65536 * sizeof(gid_t)) {
        g.resize(glen / sizeof(gid_t));
        rc = ::getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, g.data(), &glen);
    }
    if (rc == 0) g.resize(glen / sizeof(gid_t));
    else g.clear();
    p.groups = std::move(g);
#elif defined(__APPLE__)
    xucred xc{};
    socklen_t len = sizeof xc;
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &xc, &len) != 0 || xc.cr_version != XUCRED_VERSION) return p;
    p.known = true;
    p.uid = xc.cr_uid;
    p.gid = xc.cr_ngroups > 0 ? xc.cr_groups[0] : 0;
    for (short i = 0; i < xc.cr_ngroups && i < NGROUPS; ++i) p.groups.push_back(xc.cr_groups[i]);
    pid_t pid = 0;
    socklen_t plen = sizeof pid;
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &plen) == 0 && pid > 0) p.pid = pid;
#else
    (void)fd;
#endif
    return p;
}

std::string_view to_string(Access a) noexcept {
    switch (a) {
        case Access::none: return "none";
        case Access::health: return "health";
        case Access::diagnostics: return "diagnostics";
        case Access::full: return "full";
    }
    return "none";
}

Access authorize(const PeerCredentials& p, const AccessPolicy& policy) noexcept {
    if (!p.known) return Access::none;
    if (p.uid == 0 || p.uid == policy.agent_uid) return Access::full;
    if (policy.group && (p.gid == *policy.group ||
                         std::find(p.groups.begin(), p.groups.end(), *policy.group) != p.groups.end()))
        return Access::diagnostics;
    return Access::health;
}

ProcVisibility detect_proc_visibility() {
#if defined(__linux__)
    std::ifstream in("/proc/self/mountinfo");
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string id, parent, devno, root, mount_point;
        ls >> id >> parent >> devno >> root >> mount_point;
        if (mount_point != "/proc") continue;
        const auto dash = line.find(" - ");
        if (dash == std::string::npos) continue;
        std::istringstream rest(line.substr(dash + 3));
        std::string fstype, source, super_opts;
        rest >> fstype >> source >> super_opts;
        if (fstype != "proc") continue;
        std::istringstream opts(super_opts);
        for (std::string o; std::getline(opts, o, ',');) {
            if (o.rfind("hidepid=", 0) != 0) continue;
            const std::string v = o.substr(8);
            if (v != "0" && v != "off") return ProcVisibility::own_only;
        }
    }
#endif
    return ProcVisibility::all;
}

void redact(SystemState& s, uid_t peer_uid, ProcVisibility v) {
    if (!s.processes) return;
    auto& list = s.processes->value;
    if (v == ProcVisibility::own_only) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const ProcessView& p) { return p.info.uid != peer_uid; }),
                   list.end());
    }
    for (auto& p : list)
        if (p.info.uid != peer_uid) p.info.path.clear();
}

}  // namespace osie::api
