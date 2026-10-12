// SPDX-License-Identifier: Apache-2.0
// Linux: /proc/stat, /proc/loadavg, /proc/meminfo, /proc/vmstat.
#include <cstdlib>

#include "linux_os.hpp"

namespace osie::linux_os {

namespace {
bool parse_cpu_line(std::string_view line, CpuTicks& t) {
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    // guest time is already included in user/nice and must not be added again.
    std::uint64_t v[8] = {};
    std::size_t pos = line.find(' ');
    int n = 0;
    while (pos != std::string_view::npos && n < 8) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        if (pos >= line.size()) break;
        char* end = nullptr;
        const std::string tok(line.substr(pos, line.find(' ', pos) - pos));
        v[n++] = std::strtoull(tok.c_str(), &end, 10);
        pos = line.find(' ', pos);
    }
    if (n < 4) return false;
    t.user = v[0];
    t.nice = v[1];
    t.system = v[2];
    t.idle = v[3];
    t.iowait = v[4];
    t.irq = v[5];
    t.softirq = v[6];
    t.steal = v[7];
    return true;
}
}  // namespace

Result<Observation<CpuSample>> LinuxOperatingSystem::cpu_sample() {
    auto prov = Provenance::now("procfs:/proc/stat+/proc/loadavg");
    auto st = read_file(p("/proc/stat"));
    if (!st) return st.error();
    CpuSample s;
    s.has_iowait = true;
    s.has_steal = true;
    std::string_view body = *st;
    std::size_t pos = 0;
    bool agg = false;
    while (pos < body.size()) {
        const auto eol = body.find('\n', pos);
        auto line = body.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? body.size() : eol + 1;
        if (line.rfind("cpu ", 0) == 0) {
            agg = parse_cpu_line(line, s.aggregate);
        } else if (line.rfind("cpu", 0) == 0) {
            CpuTicks t;
            if (parse_cpu_line(line, t)) s.per_cpu.push_back(t);
        } else if (line.rfind("ctxt ", 0) == 0) {
            s.context_switches_total = std::strtoull(std::string(line.substr(5)).c_str(), nullptr, 10);
        } else if (line.rfind("procs_running ", 0) == 0) {
            s.runnable_tasks = static_cast<std::uint32_t>(std::strtoul(std::string(line.substr(14)).c_str(), nullptr, 10));
        } else if (line.rfind("procs_blocked ", 0) == 0) {
            s.blocked_tasks = static_cast<std::uint32_t>(std::strtoul(std::string(line.substr(14)).c_str(), nullptr, 10));
        }
    }
    if (!agg) return make_error(Errc::parse_error, "/proc/stat has no aggregate cpu line");
    if (auto la = read_file(p("/proc/loadavg"))) {
        LoadAverage l;
        if (std::sscanf(la->c_str(), "%lf %lf %lf", &l.one, &l.five, &l.fifteen) == 3) s.load = l;
    }
    // Note: procs_running includes the reading task itself; Linux load average
    // also counts tasks in uninterruptible (D) sleep, unlike XNU's.
    return Observation<CpuSample>{std::move(s), std::move(prov)};
}

Result<Observation<MemoryMetrics>> LinuxOperatingSystem::memory() {
    auto prov = Provenance::now("procfs:/proc/meminfo+/proc/vmstat");
    auto mi = read_file(p("/proc/meminfo"));
    if (!mi) return mi.error();
    MemoryMetrics m;
    m.page_size = page_size_;
    m.total_bytes = kv_field(*mi, "MemTotal", true).value_or(0);
    if (m.total_bytes == 0) return make_error(Errc::parse_error, "MemTotal missing");
    m.free_bytes = kv_field(*mi, "MemFree", true);
    m.available_bytes = kv_field(*mi, "MemAvailable", true);  // kernel's own estimate (3.14+)
    m.active_bytes = kv_field(*mi, "Active", true);
    m.inactive_bytes = kv_field(*mi, "Inactive", true);
    auto af = kv_field(*mi, "Active(file)", true);
    auto inf = kv_field(*mi, "Inactive(file)", true);
    if (af && inf) m.file_backed_bytes = *af + *inf;
    m.swap_total_bytes = kv_field(*mi, "SwapTotal", true);
    if (auto sf = kv_field(*mi, "SwapFree", true); sf && m.swap_total_bytes) m.swap_used_bytes = *m.swap_total_bytes - *sf;
    // No XNU-style compressor counter: zswap/zram are optional and accounted differently.
    if (auto vm = read_file(p("/proc/vmstat"))) {
        m.swapins = space_field(*vm, "pswpin");
        m.swapouts = space_field(*vm, "pswpout");
        m.faults = space_field(*vm, "pgfault");
        m.major_faults = space_field(*vm, "pgmajfault");
        // pgpgin/pgpgout count KiB of block I/O, not page-outs; deliberately not mapped.
    } else {
        prov.validity = Validity::partial;
        prov.note = "/proc/vmstat unreadable";
    }
    return Observation<MemoryMetrics>{m, std::move(prov)};
}

}  // namespace osie::linux_os
