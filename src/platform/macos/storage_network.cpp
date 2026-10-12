// SPDX-License-Identifier: Apache-2.0
// macOS: filesystems (getfsstat) and network interfaces (PF_ROUTE
// NET_RT_IFLIST2 for 64-bit counters; getifaddrs for addresses).
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <vector>

#include "macos_os.hpp"

namespace osie::macos {

Result<Observation<std::vector<FilesystemInfo>>> MacOperatingSystem::filesystems() {
    auto prov = Provenance::now("getfsstat(MNT_NOWAIT)");
    const int n = ::getfsstat(nullptr, 0, MNT_NOWAIT);
    if (n < 0) return make_error(errc_from_errno(errno), "getfsstat", errno);
    std::vector<struct statfs> buf(static_cast<std::size_t>(n) + 8);
    const int got = ::getfsstat(buf.data(), static_cast<int>(buf.size() * sizeof(struct statfs)), MNT_NOWAIT);
    if (got < 0) return make_error(errc_from_errno(errno), "getfsstat", errno);

    std::vector<FilesystemInfo> out;
    out.reserve(static_cast<std::size_t>(got));
    for (int i = 0; i < got; ++i) {
        const auto& s = buf[static_cast<std::size_t>(i)];
        FilesystemInfo f;
        f.mount_point = s.f_mntonname;
        f.device = s.f_mntfromname;
        f.fs_type = s.f_fstypename;
        f.total_bytes = static_cast<std::uint64_t>(s.f_blocks) * s.f_bsize;
        f.free_bytes = static_cast<std::uint64_t>(s.f_bfree) * s.f_bsize;
        f.available_bytes = static_cast<std::uint64_t>(s.f_bavail) * s.f_bsize;
        f.files_total = s.f_files;
        f.files_free = s.f_ffree;
        f.read_only = (s.f_flags & MNT_RDONLY) != 0;
        f.local = (s.f_flags & MNT_LOCAL) != 0;
        out.push_back(std::move(f));
    }
    prov.note = "APFS volumes in one container share free space";
    return Observation<std::vector<FilesystemInfo>>{std::move(out), std::move(prov)};
}

Result<Observation<std::vector<NetInterface>>> MacOperatingSystem::network_interfaces() {
    auto prov = Provenance::now("sysctl(PF_ROUTE,NET_RT_IFLIST2)+getifaddrs");
    int mib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
    std::size_t len = 0;
    if (::sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0) {
        return make_error(errc_from_errno(errno), "sysctl NET_RT_IFLIST2 size", errno);
    }
    std::vector<char> buf(len + 1024);
    len = buf.size();
    if (::sysctl(mib, 6, buf.data(), &len, nullptr, 0) != 0) {
        return make_error(errc_from_errno(errno), "sysctl NET_RT_IFLIST2", errno);
    }

    std::map<std::string, NetInterface> by_name;
    for (std::size_t off = 0; off + sizeof(if_msghdr) <= len;) {
        // Messages are packed back-to-back and are not 8-byte aligned; copy
        // into aligned locals before touching the 64-bit counters.
        if_msghdr hdr{};
        std::memcpy(&hdr, buf.data() + off, sizeof hdr);
        const auto* ifm = &hdr;
        if (ifm->ifm_msglen == 0) break;
        if (ifm->ifm_type == RTM_IFINFO2 && off + sizeof(if_msghdr2) <= len) {
            if_msghdr2 msg2{};
            std::memcpy(&msg2, buf.data() + off, sizeof msg2);
            const auto* ifm2 = &msg2;
            char name[IF_NAMESIZE] = {};
            if (::if_indextoname(ifm2->ifm_index, name) != nullptr) {
                NetInterface ni;
                ni.name = name;
                ni.up = (ifm2->ifm_flags & IFF_UP) != 0;
                ni.loopback = (ifm2->ifm_flags & IFF_LOOPBACK) != 0;
                const auto& d = ifm2->ifm_data;  // struct if_data64
                ni.mtu = d.ifi_mtu;
                ni.rx_bytes = d.ifi_ibytes;
                ni.tx_bytes = d.ifi_obytes;
                ni.rx_packets = d.ifi_ipackets;
                ni.tx_packets = d.ifi_opackets;
                ni.rx_errors = d.ifi_ierrors;
                ni.tx_errors = d.ifi_oerrors;
                ni.rx_drops = d.ifi_iqdrops;
                by_name[ni.name] = std::move(ni);
            }
        }
        off += ifm->ifm_msglen;
    }

    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (auto* a = ifa; a != nullptr; a = a->ifa_next) {
            if (a->ifa_addr == nullptr) continue;
            auto it = by_name.find(a->ifa_name);
            if (it == by_name.end()) continue;
            char host[INET6_ADDRSTRLEN] = {};
            if (a->ifa_addr->sa_family == AF_INET) {
                ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, host, sizeof host);
                it->second.addresses.emplace_back(host);
            } else if (a->ifa_addr->sa_family == AF_INET6) {
                ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(a->ifa_addr)->sin6_addr, host, sizeof host);
                it->second.addresses.emplace_back(host);
            }
        }
        ::freeifaddrs(ifa);
    } else {
        prov.validity = Validity::partial;
        prov.note = "getifaddrs failed; addresses omitted";
    }

    std::vector<NetInterface> out;
    out.reserve(by_name.size());
    for (auto& [_, ni] : by_name) out.push_back(std::move(ni));
    return Observation<std::vector<NetInterface>>{std::move(out), std::move(prov)};
}

}  // namespace osie::macos
