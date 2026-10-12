// SPDX-License-Identifier: Apache-2.0
// Integration tests against the real, running kernel. They only read state;
// the lifecycle test forks and reaps its *own* child process.
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "osie/engine/engine.hpp"
#include "osie/platform/os.hpp"

using namespace osie;

namespace {
void burn_cpu(std::chrono::milliseconds d) {
    const auto end = std::chrono::steady_clock::now() + d;
    volatile double x = 0;
    while (std::chrono::steady_clock::now() < end)
        for (int i = 0; i < 10000; ++i) x = x + i * 0.5;
}
}  // namespace

TEST(Native, PlatformMatchesBuild) {
    auto os = make_native_os();
    EXPECT_EQ(os->platform(), host_platform());
    auto sup = os->supported();
    for (auto k : {ObservationKind::system_info, ObservationKind::cpu_times, ObservationKind::memory,
                   ObservationKind::process_list})
        EXPECT_TRUE(sup.contains(k)) << to_string(k);
}

TEST(Native, SystemInfoAndTopology) {
    auto os = make_native_os();
    auto si = os->system_info();
    ASSERT_TRUE(si.ok()) << si.error().describe();
    EXPECT_FALSE(si->value.os_name.empty());
    EXPECT_FALSE(si->value.kernel_release.empty());
    EXPECT_GT(si->value.mem_total_bytes, 256ULL << 20);
    EXPECT_GT(si->value.page_size, 0U);
    EXPECT_FALSE(si->prov.source.empty());
    auto t = os->cpu_topology();
    ASSERT_TRUE(t.ok());
    EXPECT_GE(t->value.logical_cpus, 1U);
    EXPECT_GE(t->value.logical_cpus, t->value.physical_cores);
    EXPECT_EQ(t->value.logical_cpus, std::thread::hardware_concurrency());
}

TEST(Native, CpuCountersAreMonotonic) {
    auto os = make_native_os();
    auto a = os->cpu_sample();
    ASSERT_TRUE(a.ok()) << a.error().describe();
    burn_cpu(std::chrono::milliseconds(100));
    auto b = os->cpu_sample();
    ASSERT_TRUE(b.ok());
    EXPECT_GT(b->value.aggregate.total(), a->value.aggregate.total());
    EXPECT_GE(b->value.aggregate.busy(), a->value.aggregate.busy());
    EXPECT_FALSE(a->value.per_cpu.empty());
    auto u = derive_cpu_utilization(a->value, b->value);
    ASSERT_TRUE(u);
    EXPECT_GE(u->busy_pct(), 0.0);
    EXPECT_LE(u->busy_pct(), 100.0 + 1e-9);
    EXPECT_GT(b->prov.mono, a->prov.mono);
}

TEST(Native, MemoryIsPlausible) {
    auto os = make_native_os();
    auto m = os->memory();
    ASSERT_TRUE(m.ok()) << m.error().describe();
    auto si = os->system_info();
    EXPECT_EQ(m->value.total_bytes, si->value.mem_total_bytes);
    ASSERT_TRUE(m->value.available_bytes);
    EXPECT_LE(*m->value.available_bytes, m->value.total_bytes);
#if defined(__APPLE__)
    EXPECT_TRUE(m->value.compressed_bytes);
    EXPECT_TRUE(m->value.pressure_level) << "kern.memorystatus_vm_pressure_level should be readable";
#else
    EXPECT_FALSE(m->value.compressed_bytes) << "Linux has no XNU-style compressor counter";
    EXPECT_FALSE(m->value.pressure_level) << "Linux exposes PSI, not a pressure level";
#endif
}

TEST(Native, ProcessListContainsSelfWithCpuTime) {
    auto os = make_native_os();
    burn_cpu(std::chrono::milliseconds(150));
    auto snap = os->processes(ProcessQuery{}, {});
    ASSERT_TRUE(snap.ok()) << snap.error().describe();
    const ProcessInfo* self = nullptr;
    for (const auto& p : snap->value.processes)
        if (p.pid == ::getpid()) self = &p;
    ASSERT_NE(self, nullptr);
    EXPECT_EQ(self->ppid, ::getppid());
    EXPECT_EQ(self->uid, ::getuid());
    EXPECT_EQ(self->validity, Validity::valid);
    ASSERT_TRUE(self->cpu_total_ns());
    EXPECT_GE(*self->cpu_total_ns(), 100'000'000ULL) << "burned >=150 ms; units must be nanoseconds";
    EXPECT_LT(*self->cpu_total_ns(), 600'000'000'000ULL);
    ASSERT_TRUE(self->memory_bytes());
    EXPECT_GT(*self->memory_bytes(), 1ULL << 20);
    ASSERT_TRUE(self->threads);
    EXPECT_GE(*self->threads, 1U);
    ASSERT_TRUE(self->start_time_us);
    EXPECT_GE(snap->value.processes.size(), 1U);  // a container PID namespace may hold only a few
}

TEST(Native, PartialVisibilityIsReportedNotZeroed) {
    if (::geteuid() == 0) GTEST_SKIP() << "running as root: everything is readable";
    auto os = make_native_os();
    auto snap = os->processes(ProcessQuery{}, {});
    ASSERT_TRUE(snap.ok());
    for (const auto& p : snap->value.processes) {
        if (p.validity != Validity::partial) continue;
        EXPECT_FALSE(p.cpu_total_ns()) << "partial process must not report fabricated CPU time";
    }
#if defined(__APPLE__)
    EXPECT_GT(snap->value.partial_count, 0U) << "root-owned daemons are not fully readable unprivileged";
    EXPECT_EQ(snap->prov.validity, Validity::partial);
#endif
}

TEST(Native, ProcessLifecycleRace) {
    auto os = make_native_os();
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::pause();
        ::_exit(0);
    }
    auto alive = os->process(child);
    ASSERT_TRUE(alive.ok()) << alive.error().describe();
    EXPECT_EQ(alive->value.ppid, ::getpid());
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);  // reaped: the PID no longer exists
    auto gone = os->process(child);
    ASSERT_FALSE(gone.ok());
    EXPECT_TRUE(gone.error().code == Errc::process_exited || gone.error().code == Errc::not_found)
        << gone.error().describe();
    auto neg = os->process(-5);
    ASSERT_FALSE(neg.ok());
    EXPECT_EQ(neg.error().code, Errc::invalid_argument);
}

TEST(Native, FilesystemsAndNetwork) {
    auto os = make_native_os();
    auto fs = os->filesystems();
    ASSERT_TRUE(fs.ok()) << fs.error().describe();
    bool root = false;
    for (const auto& f : fs->value) {
        if (f.mount_point == "/") root = true;
        EXPECT_LE(f.available_bytes, f.total_bytes + 1);
    }
    EXPECT_TRUE(root);
    auto net = os->network_interfaces();
    ASSERT_TRUE(net.ok()) << net.error().describe();
    bool lo = false;
    for (const auto& n : net->value)
        if (n.loopback) lo = true;
    EXPECT_TRUE(lo);
}

TEST(Native, CancellationOfEnumeration) {
    auto os = make_native_os();
    CancellationSource c;
    c.cancel();
    auto r = os->processes(ProcessQuery{}, c.token());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, Errc::cancelled);
}

#if defined(__APPLE__)
TEST(NativeMac, PowerStateAndNoLinuxFeatures) {
    auto os = make_native_os();
    ASSERT_NE(os->macos(), nullptr);
    EXPECT_EQ(os->linux_ext(), nullptr);
    auto p = os->macos()->power_state();
    ASSERT_TRUE(p.ok());
    EXPECT_LE(static_cast<int>(p->value.thermal), static_cast<int>(ThermalState::critical));
    EXPECT_FALSE(os->supported().contains(ObservationKind::pressure_stall));
}
#endif

#if defined(__linux__)
TEST(NativeLinux, PsiAndCgroup) {
    auto os = make_native_os();
    ASSERT_NE(os->linux_ext(), nullptr);
    EXPECT_EQ(os->macos(), nullptr);
    auto* lx = os->linux_ext();
    if (os->supported().contains(ObservationKind::pressure_stall)) {
        auto p = lx->pressure();
        ASSERT_TRUE(p.ok()) << p.error().describe();
        EXPECT_FALSE(p->value.empty());
        for (const auto& s : p->value) {
            EXPECT_GE(s.some.avg10, 0.0);
            EXPECT_LE(s.some.avg10, 100.0);
        }
    } else {
        auto p = lx->pressure();
        EXPECT_FALSE(p.ok());
    }
    auto cg = lx->cgroup_of(::getpid());
    if (os->supported().contains(ObservationKind::cgroup)) {
        ASSERT_TRUE(cg.ok()) << cg.error().describe();
        auto info = lx->cgroup(*cg);
        ASSERT_TRUE(info.ok()) << info.error().describe();
        EXPECT_TRUE(info->value.usage_usec);
    }
}
#endif

TEST(Integration, EndToEndCpuQuestionIsGroundedAndReadOnly) {
    EngineConfig cfg;
    cfg.sample_interval = Millis{200};
    Engine e(make_native_os(), std::make_unique<intent::RuleParser>(), cfg);
    auto a = e.ask("Why is my computer using so much CPU?");
    EXPECT_EQ(a.frame.intent, intent::Intent::diagnose_cpu);
    EXPECT_TRUE(a.frame.validation_errors.empty());
    ASSERT_TRUE(a.diagnosis);
    EXPECT_NE(a.diagnosis->conclusion, "insufficient_evidence");
    ASSERT_NE(a.diagnosis->find_evidence("cpu_busy_pct"), nullptr);
    EXPECT_FALSE(a.diagnosis->missing_evidence.empty());
    for (const auto& d : e.audit().recent(100)) {
        if (d.allowed) EXPECT_EQ(d.capability.rfind("observe.", 0), 0U) << d.capability;
    }
    EXPECT_TRUE(e.graph().check_consistency().empty());
    auto b = e.ask("why is my machine slow?");
    ASSERT_TRUE(b.diagnosis);
    EXPECT_EQ(b.diagnosis->topic, "slowness");
}
