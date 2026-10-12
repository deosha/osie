// SPDX-License-Identifier: Apache-2.0
// Deterministic, scriptable IOperatingSystem for unit tests.
#pragma once

#include <deque>
#include <functional>
#include <mutex>

#include "osie/platform/os.hpp"

namespace osie::test {

inline ProcessInfo proc(Pid pid, Pid ppid, std::string name, std::uint64_t cpu_ns, std::uint64_t mem,
                        std::uint64_t start = 1000, Uid uid = 501) {
    ProcessInfo p;
    p.pid = pid;
    p.ppid = ppid;
    p.uid = uid;
    p.name = std::move(name);
    p.start_time_us = start + static_cast<std::uint64_t>(pid);
    p.cpu_user_ns = cpu_ns;
    p.cpu_system_ns = 0;
    p.resident_bytes = mem;
    p.footprint_bytes = mem;
    p.threads = 4;
    p.state = ProcessRunState::running;
    return p;
}

/// Each call to cpu_sample()/memory()/processes() pops the next scripted value
/// (the last one repeats), so a test can describe "before" and "after" states.
class MockOs final : public IOperatingSystem, public IMacExtensions, public ILinuxExtensions {
public:
    explicit MockOs(Platform p = Platform::macos) : platform_(p) {
        using K = ObservationKind;
        for (auto k : {K::system_info, K::cpu_topology, K::cpu_times, K::load_average, K::process_list,
                       K::process_detail, K::memory, K::swap, K::filesystems, K::network_interfaces}) {
            supported_.add(k);
        }
        if (p == Platform::macos) {
            supported_.add(K::thermal_state);
            supported_.add(K::memory_pressure_level);
        } else {
            supported_.add(K::pressure_stall);
            supported_.add(K::cgroup);
        }
        info.os_name = p == Platform::macos ? "macOS" : "Linux";
        info.os_version = "test";
        info.hostname = "testhost";
        info.mem_total_bytes = 16ULL << 30;
        topo.logical_cpus = 4;
        topo.physical_cores = 4;
        topo.online_cpus = 4;
        topo.clusters.push_back(CpuCluster{"cpu-package-0", 4, 4, std::nullopt});
    }

    // ---- script -------------------------------------------------------------
    SystemInfo info;
    CpuTopology topo;
    std::deque<CpuSample> cpu_script;
    std::deque<MemoryMetrics> mem_script;
    std::deque<ProcessSnapshot> proc_script;
    MacPowerState power{};
    std::vector<PressureStall> psi;
    std::vector<CgroupInfo> cgroups;
    std::vector<FilesystemInfo> fs;
    std::vector<NetInterface> nets;
    int fail_memory_with_eperm{0};
    mutable std::mutex mu;
    int process_calls{0};

    static CpuSample cpu(std::uint64_t busy_per_cpu, std::uint64_t idle_per_cpu, std::size_t n = 4,
                         std::optional<LoadAverage> load = LoadAverage{1, 1, 1}) {
        CpuSample s;
        for (std::size_t i = 0; i < n; ++i) {
            CpuTicks t;
            t.user = busy_per_cpu;
            t.idle = idle_per_cpu;
            s.per_cpu.push_back(t);
            s.aggregate.user += t.user;
            s.aggregate.idle += t.idle;
        }
        s.load = load;
        return s;
    }

    // ---- IOperatingSystem ---------------------------------------------------
    [[nodiscard]] Platform platform() const noexcept override { return platform_; }
    [[nodiscard]] ObservationSet supported() const override { return supported_; }
    void set_supported(ObservationSet s) { supported_ = s; }

    Result<Observation<SystemInfo>> system_info() override {
        return Observation<SystemInfo>{info, Provenance::now("mock.system")};
    }
    Result<Observation<CpuTopology>> cpu_topology() override {
        return Observation<CpuTopology>{topo, Provenance::now("mock.topology")};
    }
    Result<Observation<CpuSample>> cpu_sample() override {
        std::lock_guard lk(mu);
        if (cpu_script.empty()) return make_error(Errc::unavailable, "no cpu script");
        auto v = pop(cpu_script);
        return Observation<CpuSample>{v, Provenance::now("mock.cpu")};
    }
    Result<Observation<MemoryMetrics>> memory() override {
        std::lock_guard lk(mu);
        if (fail_memory_with_eperm) return make_error(Errc::permission_denied, "mock EPERM", 1);
        if (mem_script.empty()) return make_error(Errc::unavailable, "no memory script");
        return Observation<MemoryMetrics>{pop(mem_script), Provenance::now("mock.memory")};
    }
    Result<Observation<ProcessSnapshot>> processes(const ProcessQuery&, const CancellationToken& c) override {
        std::lock_guard lk(mu);
        ++process_calls;
        if (c.cancelled()) return make_error(Errc::cancelled, "cancelled");
        if (proc_script.empty()) return make_error(Errc::unavailable, "no process script");
        auto v = pop(proc_script);
        Provenance pv = Provenance::now("mock.processes");
        if (v.partial_count > 0) pv.validity = Validity::partial;
        return Observation<ProcessSnapshot>{v, pv};
    }
    Result<Observation<ProcessInfo>> process(Pid pid) override {
        std::lock_guard lk(mu);
        if (proc_script.empty()) return make_error(Errc::not_found, "none");
        for (const auto& p : proc_script.front().processes)
            if (p.pid == pid) return Observation<ProcessInfo>{p, Provenance::now("mock.process")};
        return make_error(Errc::process_exited, "gone");
    }
    Result<Observation<std::vector<FilesystemInfo>>> filesystems() override {
        return Observation<std::vector<FilesystemInfo>>{fs, Provenance::now("mock.fs")};
    }
    Result<Observation<std::vector<NetInterface>>> network_interfaces() override {
        return Observation<std::vector<NetInterface>>{nets, Provenance::now("mock.net")};
    }
    IMacExtensions* macos() noexcept override { return platform_ == Platform::macos ? this : nullptr; }
    ILinuxExtensions* linux_ext() noexcept override { return platform_ == Platform::linux_os ? this : nullptr; }
    Result<Observation<MacPowerState>> power_state() override {
        return Observation<MacPowerState>{power, Provenance::now("mock.power")};
    }
    Result<Observation<std::vector<PressureStall>>> pressure() override {
        if (psi.empty()) return make_error(Errc::unsupported, "no psi");
        return Observation<std::vector<PressureStall>>{psi, Provenance::now("mock.psi")};
    }
    Result<Observation<CgroupInfo>> cgroup(const std::string& path) override {
        for (const auto& c : cgroups)
            if (c.path == path) return Observation<CgroupInfo>{c, Provenance::now("mock.cgroup")};
        return make_error(Errc::not_found, path);
    }
    Result<std::string> cgroup_of(Pid pid) override {
        std::lock_guard lk(mu);
        if (!proc_script.empty())
            for (const auto& p : proc_script.front().processes)
                if (p.pid == pid && p.cgroup) return *p.cgroup;
        return make_error(Errc::not_found, "no cgroup");
    }

private:
    template <class T>
    static T pop(std::deque<T>& q) {
        T v = q.front();
        if (q.size() > 1) q.pop_front();
        return v;
    }
    Platform platform_;
    ObservationSet supported_;
};

}  // namespace osie::test
