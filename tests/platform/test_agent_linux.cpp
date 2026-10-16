// SPDX-License-Identifier: Apache-2.0
// osie-agent on Linux: a fixture /proc tree with unreadable and missing
// files, and the live kernel.
#include <gtest/gtest.h>

#if defined(__linux__)
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include "osie/agent/runtime.hpp"
#include "platform/linux/linux_os.hpp"

using namespace osie;
using namespace osie::agent;
using osie::linux_os::LinuxOperatingSystem;
using Clock = std::chrono::steady_clock;
extern char** environ;

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

pid_t spawn_sleeper() {
    char a0[] = "sleep", a1[] = "120";
    char* argv[] = {a0, a1, nullptr};
    pid_t pid = -1;
    return ::posix_spawnp(&pid, "sleep", nullptr, nullptr, argv, environ) == 0 ? pid : -1;
}

void reap(pid_t pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
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

TEST(AgentLinuxLive, ThousandsOfRealProcessesWithChurn) {
    // 1000 child processes, 20 of which are killed and replaced every 25 ms
    // (new PIDs, new start times), while the agent collects from /proc.
    // Sanitizer builds take seconds per collection, during which that churn
    // would replace every child: they churn 2 per 25 ms, run longer, and
    // check memory safety rather than throughput.
#if defined(OSIE_SANITIZED)
    constexpr int kChurn = 2, kSeconds = 15;
    constexpr std::size_t kMinSeen = 600, kMinCollections = 1;
#else
    constexpr int kChurn = 20, kSeconds = 5;
    constexpr std::size_t kMinSeen = 900, kMinCollections = 8;
#endif
    std::vector<pid_t> kids;
    for (int i = 0; i < 1000; ++i)
        if (const pid_t p = spawn_sleeper(); p > 0) kids.push_back(p);
    ASSERT_GE(kids.size(), 900U) << "could not spawn the workload";
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> replaced{0};
    std::thread churn([&] {
        std::size_t next = 0;
        while (!done) {
            for (int i = 0; i < kChurn; ++i, next = (next + 1) % kids.size()) {
                reap(kids[next]);
                kids[next] = spawn_sleeper();
                ++replaced;
            }
            std::this_thread::sleep_for(Millis{25});
        }
    });
    AgentConfig c = cfg();
    c.interval = Millis{250};
    c.sample_window = Millis{100};
    c.history_capacity = 16;
    c.history_top_k = 8;
    AgentRuntime rt(make_native_os(), c);
    ASSERT_TRUE(rt.start().ok());
    std::size_t max_seen = 0;
    const auto until = Clock::now() + std::chrono::seconds(kSeconds);
    while (Clock::now() < until) {
        if (auto s = rt.snapshot(); s && s->state->processes) max_seen = std::max(max_seen, s->state->processes->value.size());
        std::this_thread::sleep_for(Millis{50});
    }
    const auto t0 = Clock::now();
    rt.stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    done = true;
    churn.join();
    for (pid_t p : kids)
        if (p > 0) reap(p);
    const auto h = rt.health();
    std::printf("[ info ] %zu processes seen, %llu replaced, %llu collections, last %.1f ms, stop %.0f ms\n", max_seen,
                static_cast<unsigned long long>(replaced.load()), static_cast<unsigned long long>(h.collections),
                h.last_collection_ms, stop_ms);
    EXPECT_GE(max_seen, kMinSeen);
    EXPECT_GT(replaced.load(), 400U) << "churn did not run";
    EXPECT_GE(h.collections, kMinCollections) << to_json(h);
    EXPECT_LT(stop_ms, 1000.0);
    for (const auto& k : h.kinds)
        if (k.kind == ObservationKind::process_list || k.kind == ObservationKind::cpu_times ||
            k.kind == ObservationKind::memory)
            EXPECT_EQ(k.failed, 0U) << to_string(k.kind) << ": " << k.last_error << " (exiting processes are not failures)";
    EXPECT_LE(h.history_points, 16U);
    for (const auto& p : rt.history(100)) EXPECT_LE(p.processes.size(), 16U);
}
#endif
