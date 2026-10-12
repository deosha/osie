// SPDX-License-Identifier: Apache-2.0
// Linux: process enumeration via /proc/<pid>/{stat,status,cgroup,exe}.
#include <dirent.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <memory>

#include "linux_os.hpp"

namespace osie::linux_os {

namespace {
ProcessRunState map_state(char c) {
    switch (c) {
        case 'R': return ProcessRunState::running;
        case 'S': return ProcessRunState::sleeping;
        case 'D': return ProcessRunState::disk_wait;
        case 'T':
        case 't': return ProcessRunState::stopped;
        case 'Z': return ProcessRunState::zombie;
        case 'I': return ProcessRunState::idle;
        default: return ProcessRunState::unknown;
    }
}
}  // namespace

Result<ProcessInfo> LinuxOperatingSystem::read_process(Pid pid, bool include_path) const {
    const std::string base = p("/proc/" + std::to_string(pid));
    auto stat = read_file(base + "/stat", 4096);
    if (!stat) {
        // ENOENT/ESRCH after enumeration = the process exited (race).
        if (stat.error().code == Errc::not_found || stat.error().code == Errc::process_exited)
            return make_error(Errc::process_exited, "pid " + std::to_string(pid));
        return stat.error();
    }
    auto ps = parse_proc_stat(*stat);
    if (!ps) return make_error(Errc::parse_error, base + "/stat");
    ProcessInfo pi;
    pi.pid = pid;
    pi.ppid = ps->ppid;
    pi.name = ps->comm;
    pi.state = map_state(ps->state);
    const double tick_ns = 1e9 / static_cast<double>(clk_tck_);
    pi.cpu_user_ns = static_cast<std::uint64_t>(static_cast<double>(ps->utime_ticks) * tick_ns);
    pi.cpu_system_ns = static_cast<std::uint64_t>(static_cast<double>(ps->stime_ticks) * tick_ns);
    pi.major_faults = ps->majflt;
    pi.threads = ps->threads;
    pi.nice = ps->nice;
    pi.virtual_bytes = ps->vsize;
    pi.resident_bytes = ps->rss_pages * page_size_;
    if (boot_time_s_ > 0) {
        pi.start_time_us = boot_time_s_ * 1'000'000ULL +
                           ps->start_ticks * 1'000'000ULL / static_cast<std::uint64_t>(clk_tck_);
    } else {
        pi.start_time_us = ps->start_ticks;  // still unique per boot; documented fallback
    }
    if (auto status = read_file(base + "/status", 8192)) {
        if (auto uid = kv_field(*status, "Uid", false)) pi.uid = static_cast<Uid>(*uid);
    } else if (status.error().code == Errc::not_found) {
        return make_error(Errc::process_exited, "pid " + std::to_string(pid));
    }
    if (auto cg = read_file(base + "/cgroup", 4096)) {
        // cgroup v2 line: "0::/path"
        const auto pos = cg->find("0::");
        if (pos != std::string::npos) {
            auto eol = cg->find('\n', pos);
            pi.cgroup = cg->substr(pos + 3, eol == std::string::npos ? std::string::npos : eol - pos - 3);
        }
    }
    if (include_path) {
        char buf[4096];
        const ssize_t n = ::readlink((base + "/exe").c_str(), buf, sizeof buf - 1);
        if (n > 0) pi.path.assign(buf, static_cast<std::size_t>(n));  // EACCES for other users: path only, not partial
        // comm is truncated to 15 chars (TASK_COMM_LEN); recover from the executable name.
        if (pi.name.size() == 15 && !pi.path.empty()) {
            const auto slash = pi.path.rfind('/');
            const std::string exe = pi.path.substr(slash + 1);
            if (exe.size() > pi.name.size() && exe.compare(0, pi.name.size(), pi.name) == 0) pi.name = exe;
        }
    }
    return pi;
}

Result<Observation<ProcessSnapshot>> LinuxOperatingSystem::processes(const ProcessQuery& q,
                                                                     const CancellationToken& cancel) {
    auto prov = Provenance::now("procfs:/proc/<pid>/stat");
    std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir(p("/proc").c_str()), ::closedir);
    if (!dir) return make_error(errc_from_errno(errno), "/proc", errno);
    ProcessSnapshot snap;
    while (auto* ent = ::readdir(dir.get())) {
        if (cancel.cancelled()) return make_error(Errc::cancelled, "process enumeration cancelled");
        const char* n = ent->d_name;
        if (!std::isdigit(static_cast<unsigned char>(n[0]))) continue;
        const Pid pid = static_cast<Pid>(std::strtol(n, nullptr, 10));
        auto r = read_process(pid, q.include_paths);
        if (!r) {
            if (r.error().code == Errc::process_exited) ++snap.vanished_count;
            continue;
        }
        // Kernel threads are children of kthreadd (pid 2) and have no user memory.
        if (!q.include_kernel_threads && (r->ppid == 2 || r->pid == 2)) continue;
        snap.processes.push_back(std::move(r).value());
    }
    return Observation<ProcessSnapshot>{std::move(snap), std::move(prov)};
}

Result<Observation<ProcessInfo>> LinuxOperatingSystem::process(Pid pid) {
    if (pid <= 0) return make_error(Errc::invalid_argument, "pid must be positive");
    auto prov = Provenance::now("procfs:/proc/<pid>/stat");
    auto r = read_process(pid, true);
    if (!r) return r.error();
    return Observation<ProcessInfo>{std::move(r).value(), std::move(prov)};
}

}  // namespace osie::linux_os
