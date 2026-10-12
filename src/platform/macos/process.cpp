// SPDX-License-Identifier: Apache-2.0
// macOS: process enumeration and inspection via libproc.
//
// Permission model observed on macOS 15 (unprivileged user): PROC_PIDT_SHORTBSDINFO
// (name, ppid, uid, status) succeeds for every process, while PROC_PIDTASKALLINFO
// and proc_pid_rusage (CPU time, memory) fail with EPERM for processes owned by
// other users. Such processes are reported as Validity::partial with the CPU and
// memory fields left empty -- never as zero.
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "macos_os.hpp"

namespace osie::macos {
namespace {

ProcessRunState map_status(std::uint32_t st) {
    switch (st) {
        case SIDL: return ProcessRunState::idle;
        case SRUN: return ProcessRunState::running;
        case SSLEEP: return ProcessRunState::sleeping;
        case SSTOP: return ProcessRunState::stopped;
        case SZOMB: return ProcessRunState::zombie;
        default: return ProcessRunState::unknown;
    }
}

bool process_exists(Pid pid) {
    // proc_pidinfo returns 0 with errno ESRCH for a vanished PID.
    struct proc_bsdshortinfo s {};
    return ::proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &s, sizeof s) == sizeof s || errno != ESRCH;
}

}  // namespace

Result<ProcessInfo> MacOperatingSystem::read_process(Pid pid, bool include_path) const {
    ProcessInfo p;
    p.pid = pid;

    struct proc_bsdshortinfo sb {};
    errno = 0;
    if (::proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &sb, sizeof sb) != sizeof sb) {
        const int err = errno;
        if (err == ESRCH || err == 0) return make_error(Errc::process_exited, "pid " + std::to_string(pid), err);
        return make_error(errc_from_errno(err), "PROC_PIDT_SHORTBSDINFO pid " + std::to_string(pid), err);
    }
    p.ppid = static_cast<Pid>(sb.pbsi_ppid);
    p.uid = sb.pbsi_uid;
    p.name.assign(sb.pbsi_comm, ::strnlen(sb.pbsi_comm, sizeof sb.pbsi_comm));
    p.state = map_status(sb.pbsi_status);

    struct proc_taskallinfo ti {};
    errno = 0;
    if (::proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &ti, sizeof ti) == sizeof ti) {
        // pbi_name holds up to 2*MAXCOMLEN chars; prefer it over the 16-char comm.
        const std::size_t n = ::strnlen(ti.pbsd.pbi_name, sizeof ti.pbsd.pbi_name);
        if (n > 0) p.name.assign(ti.pbsd.pbi_name, n);
        p.start_time_us = static_cast<std::uint64_t>(ti.pbsd.pbi_start_tvsec) * 1'000'000ULL +
                          ti.pbsd.pbi_start_tvusec;
        p.nice = ti.pbsd.pbi_nice;
        // Units are Mach absolute time on Apple Silicon (verified against getrusage).
        p.cpu_user_ns = timebase_.to_ns(ti.ptinfo.pti_total_user);
        p.cpu_system_ns = timebase_.to_ns(ti.ptinfo.pti_total_system);
        p.resident_bytes = ti.ptinfo.pti_resident_size;
        p.virtual_bytes = ti.ptinfo.pti_virtual_size;
        p.threads = static_cast<std::uint32_t>(ti.ptinfo.pti_threadnum);
        p.major_faults = static_cast<std::uint64_t>(ti.ptinfo.pti_pageins);

        rusage_info_v4 ru{};
        if (::proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ru)) == 0) {
            p.footprint_bytes = ru.ri_phys_footprint;
        }
    } else {
        const int err = errno;
        if (err == ESRCH || !process_exists(pid)) {
            return make_error(Errc::process_exited, "pid " + std::to_string(pid), err);
        }
        p.validity = Validity::partial;  // typically EPERM: other user's process
    }

    if (include_path) {
        char buf[PROC_PIDPATHINFO_MAXSIZE];
        if (::proc_pidpath(pid, buf, sizeof buf) > 0) {
            p.path = buf;
            // The kernel truncates names to 2*MAXCOMLEN; recover the full name from the executable.
            const auto slash = p.path.rfind('/');
            const std::string base = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
            if (p.name.size() >= 15 && base.size() > p.name.size() && base.compare(0, p.name.size(), p.name) == 0) {
                p.name = base;
            }
        }
    }
    return p;
}

Result<Observation<ProcessSnapshot>> MacOperatingSystem::processes(const ProcessQuery& q,
                                                                   const CancellationToken& cancel) {
    auto prov = Provenance::now("libproc.proc_listallpids+proc_pidinfo");
    // The PID list can grow between the sizing call and the fill call; retry with headroom.
    std::vector<pid_t> pids;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int n = ::proc_listallpids(nullptr, 0);
        if (n <= 0) return make_error(Errc::unavailable, "proc_listallpids", errno);
        pids.resize(static_cast<std::size_t>(n) + 64);
        const int got = ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        if (got <= 0) return make_error(Errc::unavailable, "proc_listallpids", errno);
        if (static_cast<std::size_t>(got) < pids.size()) {
            pids.resize(static_cast<std::size_t>(got));
            break;
        }
    }

    ProcessSnapshot snap;
    snap.processes.reserve(pids.size());
    for (pid_t pid : pids) {
        if (cancel.cancelled()) return make_error(Errc::cancelled, "process enumeration cancelled");
        auto r = read_process(pid, q.include_paths);
        if (!r) {
            if (r.error().code == Errc::process_exited) ++snap.vanished_count;
            continue;  // pid 0 (kernel_task) is not readable unprivileged
        }
        if (r->validity == Validity::partial) ++snap.partial_count;
        snap.processes.push_back(std::move(r).value());
    }
    if (snap.partial_count > 0) {
        prov.validity = Validity::partial;
        prov.note = std::to_string(snap.partial_count) +
                    " processes owned by other users: CPU/memory not readable without privileges";
    }
    return Observation<ProcessSnapshot>{std::move(snap), std::move(prov)};
}

Result<Observation<ProcessInfo>> MacOperatingSystem::process(Pid pid) {
    if (pid < 0) return make_error(Errc::invalid_argument, "negative pid");
    auto prov = Provenance::now("libproc.proc_pidinfo");
    auto r = read_process(pid, true);
    if (!r) return r.error();
    prov.validity = r->validity;
    return Observation<ProcessInfo>{std::move(r).value(), std::move(prov)};
}

}  // namespace osie::macos
