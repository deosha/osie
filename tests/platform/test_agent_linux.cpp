// SPDX-License-Identifier: Apache-2.0
// osie-agent on Linux: a fixture /proc tree with unreadable and missing
// files, and the live kernel.
#include <gtest/gtest.h>

#if defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <thread>

#include "osie/agent/runtime.hpp"
#include "platform/linux/linux_os.hpp"

using namespace osie;
using namespace osie::agent;
using osie::linux_os::LinuxOperatingSystem;
using Clock = std::chrono::steady_clock;

namespace {

void put(const std::string& path, const std::string& body, mode_t mode = 0644) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path) << body;
    ::chmod(path.c_str(), mode);
}

std::string proc_stat(int pid, const char* comm, unsigned long utime) {
    // pid (comm) state ppid ... utime(14) stime(15) ... starttime(22) vsize(23) rss(24)
    std::string s = std::to_string(pid) + " (" + comm + ") S 1 1 1 0 -1 4194560 100 0 0 0 " + std::to_string(utime) +
                    " 5 0 0 20 0 1 0 " + std::to_string(1000 + pid) + " 10000000 250 18446744073709551615";
    for (int i = 0; i < 27; ++i) s += " 0";
    return s + "\n";
}

const KindHealth* kind(const HealthStatus& h, ObservationKind k) {
    for (const auto& x : h.kinds)
        if (x.kind == k) return &x;
    return nullptr;
}

AgentConfig cfg() {
    AgentConfig c;
    c.interval = Millis{50};
    c.sample_window = Millis{10};
    c.history_capacity = 8;
    return c;
}

}  // namespace

TEST(AgentLinuxFixture, UnreadableProcEntriesAndMissingPsiDoNotStopCollection) {
    const std::string root = ::testing::TempDir() + "osie_agent_fixture_" + std::to_string(::getpid());
    put(root + "/proc/stat", "cpu  100 0 50 1000 5 0 0 0 0 0\ncpu0 100 0 50 1000 5 0 0 0 0 0\nbtime 1700000000\n"
                             "ctxt 42\nprocs_running 1\nprocs_blocked 0\n");
    put(root + "/proc/loadavg", "0.10 0.20 0.30 1/50 999\n");
    put(root + "/proc/meminfo", "MemTotal: 8000000 kB\nMemFree: 1000000 kB\nMemAvailable: 4000000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
    put(root + "/proc/100/stat", proc_stat(100, "readable", 50));
    put(root + "/proc/100/status", "Name:\treadable\nUid:\t1000\t1000\t1000\t1000\nVmRSS:\t2048 kB\nThreads:\t1\n");
    put(root + "/proc/200/stat", proc_stat(200, "secret", 70));
    put(root + "/proc/200/status", "Name:\tsecret\nUid:\t0\t0\t0\t0\n", 0000);  // another user's process
    put(root + "/proc/300/stat", "garbage that is not a stat line\n");          // corrupt
    // No /proc/pressure: a kernel without PSI. No cgroup2 mount.
    auto os = std::make_unique<LinuxOperatingSystem>(root);
    EXPECT_FALSE(os->supported().contains(ObservationKind::pressure_stall));
    AgentRuntime rt(std::move(os), cfg());
    for (int i = 0; i < 4; ++i) rt.collect_now();
    const auto h = rt.health();
    EXPECT_EQ(h.collections, 4U);
    const auto* psi = kind(h, ObservationKind::pressure_stall);
    ASSERT_NE(psi, nullptr);
    EXPECT_FALSE(psi->supported);
    EXPECT_EQ(psi->failed, 0U);
    const auto snap = rt.snapshot();
    ASSERT_TRUE(snap && snap->state->processes) << to_json(h);
    const auto* readable = snap->state->find_pid(100);
    ASSERT_NE(readable, nullptr);
    EXPECT_EQ(readable->info.name, "readable");
    EXPECT_EQ(snap->state->find_pid(300), nullptr) << "a corrupt stat line is skipped, not misparsed";
    // Unprivileged, the 0000 status file cannot be read. CPU and memory come
    // from the stat line, so the process is still listed with its name.
    const auto* secret = snap->state->find_pid(200);
    ASSERT_NE(secret, nullptr);
    EXPECT_EQ(secret->info.name, "secret");
    EXPECT_TRUE(secret->cpu_pct.has_value());
    std::filesystem::remove_all(root);
}

TEST(AgentLinuxLive, CollectsFromTheRunningKernelAndStopsPromptly) {
    AgentConfig c = cfg();
    c.interval = Millis{250};
    c.sample_window = Millis{100};
    AgentRuntime rt(make_native_os(), c);
    ASSERT_TRUE(rt.start().ok());
    const auto until = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < until && rt.health().collections < 4) std::this_thread::sleep_for(Millis{20});
    const auto t0 = Clock::now();
    rt.stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const auto h = rt.health();
    EXPECT_GE(h.collections, 4U) << to_json(h);
    EXPECT_LT(stop_ms, 1000.0);
    const auto s = rt.snapshot();
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->state->cpu.has_value());
    EXPECT_TRUE(s->state->memory.has_value());
    ASSERT_TRUE(s->state->processes.has_value());
    EXPECT_NE(s->state->find_pid(::getpid()), nullptr) << "the agent sees its own process";
    for (const auto& k : h.kinds)
        if (k.supported && k.kind != ObservationKind::cgroup)
            EXPECT_EQ(k.consecutive_failures, 0U) << to_string(k.kind) << ": " << k.last_error;
    EXPECT_LE(h.history_points, 8U);
}
#endif
