// SPDX-License-Identifier: Apache-2.0
// A Linux-like mock machine for agent and API tests: monotonically
// increasing CPU counters, a few processes of two users with growing CPU
// times, PSI, and one CPU-limited cgroup.
#pragma once

#include <chrono>
#include <memory>

#include "osie/agent/runtime.hpp"
#include "support/mock_os.hpp"

namespace osie::test {

inline constexpr Uid kAlice = 1000;  // owns nginx and the worker
inline constexpr Uid kBob = 1001;    // owns postgres

inline ProcessInfo owned(Pid pid, const char* name, Uid uid, std::uint64_t cpu_ns, std::uint64_t mem, const char* path) {
    auto p = proc(pid, 1, name, cpu_ns, mem, 1000, uid);
    p.path = path;
    return p;
}

/// Processes at call `n` of processes(): nginx (alice) is the busiest,
/// postgres (bob) uses the most memory. CPU times grow with real time at a
/// fixed share of one CPU each (nginx 50%, postgres 15%, worker 5%), so
/// per-process rates are consistent with the host's 30% of 4 CPUs whatever
/// the sampling window is.
inline ProcessSnapshot machine_processes(int /*n*/) {
    static const auto t0 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ProcessSnapshot s;
    s.processes = {owned(1, "init", 0, 0, 8ULL << 20, "/sbin/init"),
                   owned(100, "nginx", kAlice, ns / 2, 300ULL << 20, "/usr/sbin/nginx"),
                   owned(101, "worker", kAlice, ns / 20, 50ULL << 20, "/opt/app/worker"),
                   owned(200, "postgres", kBob, ns * 3 / 20, 3000ULL << 20, "/usr/lib/postgresql/bin/postgres")};
    s.processes[1].cgroup = "/system.slice/nginx.service";
    return s;
}

inline std::unique_ptr<MockOs> machine() {
    auto os = std::make_unique<MockOs>(Platform::linux_os);
    os->cpu_gen = [](int i) {
        return MockOs::cpu(300ULL * static_cast<std::uint64_t>(i), 700ULL * static_cast<std::uint64_t>(i));
    };
    MemoryMetrics m;
    m.total_bytes = 16ULL << 30;
    m.available_bytes = 6ULL << 30;
    m.swap_total_bytes = 0;
    os->mem_script = {m};
    os->proc_gen = machine_processes;
    PressureStall cpu;
    cpu.resource = PsiResource::cpu;
    cpu.some.avg10 = 12.5;
    PressureStall mem;
    mem.resource = PsiResource::memory;
    mem.some.avg10 = 0.5;
    os->psi = {cpu, mem};
    CgroupInfo cg;
    cg.path = "/system.slice/nginx.service";
    cg.cpu_quota_us = 50000;
    cg.cpu_period_us = 100000;
    cg.nr_periods = 1000;
    cg.nr_throttled = 300;
    cg.throttled_usec = 1'000'000;
    cg.usage_usec = 4'000'000;
    os->cgroups = {cg};
    // cpu.stat advances with every read: 10 periods, 4 of them throttled.
    os->cgroup_gen = [cg](const std::string& path, int n) {
        if (path != cg.path) return CgroupInfo{};
        CgroupInfo c = cg;
        const auto k = static_cast<std::uint64_t>(n);
        c.nr_periods = 1000 + 10 * k;
        c.nr_throttled = 300 + 4 * k;
        c.usage_usec = 4'000'000 + 50'000 * k;
        c.throttled_usec = 1'000'000 + 20'000 * k;
        return c;
    };
    return os;
}

inline agent::AgentConfig fast_config() {
    agent::AgentConfig c;  // built directly: tests may use intervals below the file minimum
    c.interval = Millis{50};
    c.sample_window = Millis{5};
    c.history_capacity = 64;
    c.history_top_k = 4;
    c.cgroup_top_n = 4;
    return c;
}

}  // namespace osie::test
