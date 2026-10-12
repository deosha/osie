// SPDX-License-Identifier: Apache-2.0
// Linux: mounted filesystems (/proc/self/mounts + statvfs) and network links
// via rtnetlink RTM_GETLINK (IFLA_STATS64), falling back to /proc/net/dev.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <set>

#include "linux_os.hpp"

namespace osie::linux_os {

namespace {
std::string unescape_mount(std::string_view s) {
    // /proc/mounts escapes space, tab, newline and backslash as octal (\040 ...).
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            const std::string oct(s.substr(i + 1, 3));
            char* end = nullptr;
            const long v = std::strtol(oct.c_str(), &end, 8);
            if (end == oct.c_str() + 3) {
                o += static_cast<char>(v);
                i += 3;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) ::close(fd_);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};
}  // namespace

Result<Observation<std::vector<FilesystemInfo>>> LinuxOperatingSystem::filesystems() {
    auto prov = Provenance::now("procfs:/proc/self/mounts+statvfs(3)");
    auto mounts = read_file(p("/proc/self/mounts"));
    if (!mounts) return mounts.error();
    static const std::set<std::string, std::less<>> kPseudo = {
        "proc", "sysfs", "devpts", "mqueue", "cgroup", "cgroup2", "securityfs", "debugfs", "tracefs", "pstore",
        "bpf", "configfs", "fusectl", "hugetlbfs", "autofs", "binfmt_misc", "devtmpfs", "rpc_pipefs", "nsfs"};
    std::vector<FilesystemInfo> out;
    std::set<std::string> seen;
    std::string_view body = *mounts;
    std::size_t pos = 0;
    std::uint32_t failures = 0;
    while (pos < body.size()) {
        const auto eol = body.find('\n', pos);
        auto line = body.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? body.size() : eol + 1;
        std::string_view f[4];
        std::size_t s = 0;
        for (int i = 0; i < 4; ++i) {
            const auto sp = line.find(' ', s);
            f[i] = line.substr(s, sp == std::string_view::npos ? std::string_view::npos : sp - s);
            if (sp == std::string_view::npos) break;
            s = sp + 1;
        }
        if (f[2].empty() || kPseudo.count(f[2])) continue;
        const std::string mp = unescape_mount(f[1]);
        if (!seen.insert(mp).second) continue;
        struct statvfs sv {};
        if (::statvfs((root_ + mp).c_str(), &sv) != 0) {
            ++failures;
            continue;
        }
        FilesystemInfo fi;
        fi.mount_point = mp;
        fi.device = unescape_mount(f[0]);
        fi.fs_type = std::string(f[2]);
        fi.total_bytes = static_cast<std::uint64_t>(sv.f_blocks) * sv.f_frsize;
        fi.free_bytes = static_cast<std::uint64_t>(sv.f_bfree) * sv.f_frsize;
        fi.available_bytes = static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize;
        fi.files_total = sv.f_files;
        fi.files_free = sv.f_ffree;
        fi.read_only = (sv.f_flag & ST_RDONLY) != 0;
        fi.local = fi.fs_type != "nfs" && fi.fs_type != "nfs4" && fi.fs_type != "cifs" && fi.fs_type != "fuse.sshfs";
        out.push_back(std::move(fi));
    }
    if (failures > 0) {
        prov.validity = Validity::partial;
        prov.note = std::to_string(failures) + " mounts could not be stat'ed (permission or stale)";
    }
    return Observation<std::vector<FilesystemInfo>>{std::move(out), std::move(prov)};
}

// The rtnetlink NLMSG_*/RTA_* macros are C-style casts by design.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
Result<std::vector<NetInterface>> LinuxOperatingSystem::netlink_links() const {
    Socket sock(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
    if (sock.get() < 0) return make_error(errc_from_errno(errno), "netlink socket", errno);
    struct {
        nlmsghdr nh;
        ifinfomsg ifm;
    } req{};
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    req.nh.nlmsg_type = RTM_GETLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nh.nlmsg_seq = 1;
    req.ifm.ifi_family = AF_UNSPEC;
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (::sendto(sock.get(), &req, req.nh.nlmsg_len, 0, reinterpret_cast<sockaddr*>(&kernel), sizeof kernel) < 0)
        return make_error(errc_from_errno(errno), "netlink sendto", errno);

    std::vector<NetInterface> out;
    std::vector<char> buf(32768);
    for (int guard = 0; guard < 1024; ++guard) {
        const ssize_t n = ::recv(sock.get(), buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return make_error(errc_from_errno(errno), "netlink recv", errno);
        }
        auto len = static_cast<unsigned>(n);
        for (auto* nh = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE) return out;
            if (nh->nlmsg_type == NLMSG_ERROR) return make_error(Errc::io_error, "netlink error reply");
            if (nh->nlmsg_type != RTM_NEWLINK) continue;
            auto* ifm = static_cast<ifinfomsg*>(NLMSG_DATA(nh));
            NetInterface ni;
            ni.up = (ifm->ifi_flags & IFF_UP) != 0;
            ni.loopback = (ifm->ifi_flags & IFF_LOOPBACK) != 0;
            int alen = static_cast<int>(IFLA_PAYLOAD(nh));
            for (auto* a = IFLA_RTA(ifm); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type == IFLA_IFNAME) {
                    ni.name = static_cast<const char*>(RTA_DATA(a));
                } else if (a->rta_type == IFLA_MTU && RTA_PAYLOAD(a) >= sizeof(std::uint32_t)) {
                    std::uint32_t mtu = 0;
                    std::memcpy(&mtu, RTA_DATA(a), sizeof mtu);
                    ni.mtu = mtu;
                } else if (a->rta_type == IFLA_STATS64 && RTA_PAYLOAD(a) >= sizeof(rtnl_link_stats64)) {
                    rtnl_link_stats64 st{};
                    std::memcpy(&st, RTA_DATA(a), sizeof st);  // attribute payload may be unaligned
                    ni.rx_bytes = st.rx_bytes;
                    ni.tx_bytes = st.tx_bytes;
                    ni.rx_packets = st.rx_packets;
                    ni.tx_packets = st.tx_packets;
                    ni.rx_errors = st.rx_errors;
                    ni.tx_errors = st.tx_errors;
                    ni.rx_drops = st.rx_dropped;
                }
            }
            if (!ni.name.empty()) out.push_back(std::move(ni));
        }
    }
    return make_error(Errc::io_error, "netlink dump did not terminate");
}

#pragma GCC diagnostic pop

Result<std::vector<NetInterface>> LinuxOperatingSystem::procfs_links() const {
    auto dev = read_file(p("/proc/net/dev"));
    if (!dev) return dev.error();
    std::vector<NetInterface> out;
    std::string_view body = *dev;
    std::size_t pos = 0;
    int line_no = 0;
    while (pos < body.size()) {
        const auto eol = body.find('\n', pos);
        auto line = body.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? body.size() : eol + 1;
        if (++line_no <= 2) continue;  // headers
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        NetInterface ni;
        auto name = line.substr(0, colon);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        ni.name = std::string(name);
        unsigned long long v[16] = {};
        const std::string rest(line.substr(colon + 1));
        if (std::sscanf(rest.c_str(), "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                        &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]) < 11)
            continue;
        ni.rx_bytes = v[0];
        ni.rx_packets = v[1];
        ni.rx_errors = v[2];
        ni.rx_drops = v[3];
        ni.tx_bytes = v[8];
        ni.tx_packets = v[9];
        ni.tx_errors = v[10];
        ni.loopback = ni.name == "lo";
        ni.up = true;  // /proc/net/dev has no flags; refined below via getifaddrs
        out.push_back(std::move(ni));
    }
    return out;
}

Result<Observation<std::vector<NetInterface>>> LinuxOperatingSystem::network_interfaces() {
    auto prov = Provenance::now("rtnetlink:RTM_GETLINK(IFLA_STATS64)+getifaddrs");
    auto links = netlink_links();
    if (!links) {
        links = procfs_links();
        if (!links) return links.error();
        prov.source = "procfs:/proc/net/dev+getifaddrs";
        prov.note = "netlink unavailable; used /proc/net/dev";
    }
    std::map<std::string, std::size_t> idx;
    for (std::size_t i = 0; i < links->size(); ++i) idx[(*links)[i].name] = i;
    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (auto* a = ifa; a != nullptr; a = a->ifa_next) {
            auto it = idx.find(a->ifa_name);
            if (it == idx.end() || a->ifa_addr == nullptr) continue;
            auto& ni = (*links)[it->second];
            char host[INET6_ADDRSTRLEN] = {};
            if (a->ifa_addr->sa_family == AF_INET) {
                ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, host, sizeof host);
                ni.addresses.emplace_back(host);
            } else if (a->ifa_addr->sa_family == AF_INET6) {
                ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(a->ifa_addr)->sin6_addr, host, sizeof host);
                ni.addresses.emplace_back(host);
            }
        }
        ::freeifaddrs(ifa);
    }
    return Observation<std::vector<NetInterface>>{std::move(links).value(), std::move(prov)};
}

}  // namespace osie::linux_os
