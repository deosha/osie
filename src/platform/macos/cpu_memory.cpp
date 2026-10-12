// SPDX-License-Identifier: Apache-2.0
// macOS: CPU tick counters (host_processor_info) and VM statistics
// (host_statistics64, vm.swapusage, kern.memorystatus_vm_pressure_level).
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <stdlib.h>  // getloadavg
#include <sys/sysctl.h>

#include "macos_os.hpp"

namespace osie::macos {

Result<Observation<CpuSample>> MacOperatingSystem::cpu_sample() {
    natural_t cpu_count = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t info_count = 0;
    const kern_return_t kr =
        ::host_processor_info(host_, PROCESSOR_CPU_LOAD_INFO, &cpu_count, &info, &info_count);
    auto prov = Provenance::now("mach.host_processor_info(PROCESSOR_CPU_LOAD_INFO)");
    if (kr != KERN_SUCCESS) {
        return make_error(Errc::unavailable, std::string("host_processor_info: ") + ::mach_error_string(kr));
    }
    // RAII release of the kernel-allocated array.
    struct VmGuard {
        processor_info_array_t p;
        mach_msg_type_number_t n;
        ~VmGuard() {
            ::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(p),
                            static_cast<vm_size_t>(n) * sizeof(integer_t));
        }
    } guard{info, info_count};

    CpuSample s;
    s.per_cpu.reserve(cpu_count);
    const auto* load = reinterpret_cast<const processor_cpu_load_info_data_t*>(info);
    for (natural_t i = 0; i < cpu_count; ++i) {
        CpuTicks t;
        t.user = load[i].cpu_ticks[CPU_STATE_USER];
        t.system = load[i].cpu_ticks[CPU_STATE_SYSTEM];
        t.idle = load[i].cpu_ticks[CPU_STATE_IDLE];
        t.nice = load[i].cpu_ticks[CPU_STATE_NICE];
        s.aggregate.user += t.user;
        s.aggregate.system += t.system;
        s.aggregate.idle += t.idle;
        s.aggregate.nice += t.nice;
        s.per_cpu.push_back(t);
    }
    // XNU does not account iowait/steal/irq separately.
    s.has_iowait = false;
    s.has_steal = false;
    double la[3] = {0, 0, 0};
    if (::getloadavg(la, 3) == 3) s.load = LoadAverage{la[0], la[1], la[2]};
    // Note: XNU's load average counts runnable+running threads (not D-state
    // tasks as on Linux), sampled by the scheduler every 5 s.
    return Observation<CpuSample>{std::move(s), std::move(prov)};
}

Result<Observation<MemoryMetrics>> MacOperatingSystem::memory() {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const kern_return_t kr =
        ::host_statistics64(host_, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
    auto prov = Provenance::now("mach.host_statistics64(HOST_VM_INFO64)+sysctl(vm.swapusage)");
    if (kr != KERN_SUCCESS) {
        return make_error(Errc::unavailable, std::string("host_statistics64: ") + ::mach_error_string(kr));
    }
    vm_size_t page = 0;
    if (::host_page_size(host_, &page) != KERN_SUCCESS || page == 0) page = vm_kernel_page_size;
    const std::uint64_t ps = page;

    MemoryMetrics m;
    m.page_size = ps;
    m.total_bytes = sysctl_u64("hw.memsize").value_or(0);
    m.free_bytes = (static_cast<std::uint64_t>(vm.free_count) - vm.speculative_count) * ps;
    m.active_bytes = static_cast<std::uint64_t>(vm.active_count) * ps;
    m.inactive_bytes = static_cast<std::uint64_t>(vm.inactive_count) * ps;
    m.wired_bytes = static_cast<std::uint64_t>(vm.wire_count) * ps;
    m.compressed_bytes = static_cast<std::uint64_t>(vm.compressor_page_count) * ps;
    m.file_backed_bytes = static_cast<std::uint64_t>(vm.external_page_count) * ps;
    // Estimate of memory obtainable without paging: free + speculative + purgeable
    // + file-backed (clean, reclaimable) pages. Heuristic; flagged in the note.
    m.available_bytes = (static_cast<std::uint64_t>(vm.free_count) + vm.purgeable_count +
                         vm.external_page_count) * ps;
    m.pageins = vm.pageins;
    m.pageouts = vm.pageouts;
    m.swapins = vm.swapins;
    m.swapouts = vm.swapouts;
    m.compressions = vm.compressions;
    m.decompressions = vm.decompressions;
    m.faults = vm.faults;

    struct xsw_usage sw {};
    std::size_t len = sizeof sw;
    if (::sysctlbyname("vm.swapusage", &sw, &len, nullptr, 0) == 0) {
        m.swap_total_bytes = sw.xsu_total;
        m.swap_used_bytes = sw.xsu_used;
    }
    // Documented values: 1 = normal, 2 = warning, 4 = critical.
    if (auto lvl = sysctl_u64("kern.memorystatus_vm_pressure_level")) {
        switch (*lvl) {
            case 1: m.pressure_level = MemoryPressureLevel::normal; break;
            case 2: m.pressure_level = MemoryPressureLevel::warning; break;
            case 4: m.pressure_level = MemoryPressureLevel::critical; break;
            default: break;
        }
    }
    prov.note = "available_bytes is an estimate (free+purgeable+file-backed)";
    return Observation<MemoryMetrics>{std::move(m), std::move(prov)};
}

}  // namespace osie::macos
