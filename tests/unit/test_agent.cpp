// SPDX-License-Identifier: Apache-2.0
// osie-agent runtime (Feature 1): configuration validation, lifecycle,
// cancellation, failure handling, identity, bounds and concurrency.
#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <set>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "osie/agent/runtime.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::agent;
using osie::test::MockOs;
using osie::test::proc;
using Clock = std::chrono::steady_clock;

namespace {

AgentConfig fast_cfg(Millis interval = Millis{20}, Millis window = Millis{2}) {
    AgentConfig c;  // constructed directly: tests may use intervals below the config-file minimum
    c.interval = interval;
    c.sample_window = window;
    c.history_capacity = 16;
    c.history_top_k = 2;
    c.cgroup_top_n = 2;
    return c;
}

ProcessSnapshot snap(std::vector<ProcessInfo> v) {
    ProcessSnapshot s;
    s.processes = std::move(v);
    return s;
}

std::unique_ptr<MockOs> linux_os() {
    auto os = std::make_unique<MockOs>(Platform::linux_os);
    for (std::uint64_t i = 0; i < 4000; ++i) os->cpu_script.push_back(MockOs::cpu(100 * i, 1000 + 100 * i));
    MemoryMetrics m;
    m.total_bytes = 8ULL << 30;
    m.available_bytes = 4ULL << 30;
    os->mem_script = {m};
    os->proc_script = {snap({proc(1, 0, "init", 0, 1 << 20), proc(40, 1, "web", 1'000'000, 50 << 20)})};
    PressureStall p;
    p.resource = PsiResource::cpu;
    os->psi = {p};
    return os;
}

std::string write_tmp(const std::string& name, const std::string& body, mode_t mode = 0600) {
    const std::string path = ::testing::TempDir() + "osie_agent_" + std::to_string(::getpid()) + "_" + name;
    std::ofstream(path) << body;
    ::chmod(path.c_str(), mode);
    return path;
}

const KindHealth* kind(const HealthStatus& h, ObservationKind k) {
    for (const auto& x : h.kinds)
        if (x.kind == k) return &x;
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------ configuration ---

TEST(AgentConfig, DefaultsRoundTripAndValidParse) {
    const AgentConfig d;
    auto r = parse_agent_config(to_text(d));
    ASSERT_TRUE(r.ok()) << r.error().describe();
    EXPECT_EQ(r->interval, d.interval);
    EXPECT_EQ(r->observe, d.observe);
    auto c = parse_agent_config("# comment\ninterval = 2m\nsample_window = 1s  # trailing\nhistory_capacity=100\n"
                                "observe = cpu_times, memory ,process_list\nlog_level = debug\n");
    ASSERT_TRUE(c.ok()) << c.error().describe();
    EXPECT_EQ(c->interval, Millis{120'000});
    EXPECT_EQ(c->sample_window, Millis{1000});
    EXPECT_EQ(c->history_capacity, 100U);
    EXPECT_EQ(c->observe.size(), 3U);
    EXPECT_EQ(c->log_level, LogLevel::debug);
}

TEST(AgentConfig, InvalidValuesAreRejected) {
    for (const char* bad : {
             "interval = 100ms",              // below 250 ms
             "interval = 2h",                 // above 1 h
             "interval = 5",                  // no unit
             "interval = 5 s",                // space inside the value
             "interval = -5s",
             "interval = 5d",
             "sample_window = 10ms",
             "interval = 1s\nsample_window = 600ms",  // more than half the interval
             "history_capacity = 0",
             "history_capacity = 100001",
             "history_capacity = 1e3",
             "history_top_k = 65",
             "cgroup_top_n = 65",
             "failure_threshold = 0",
             "log_level = verbose",
             "observe = cpu_times,gpu",
             "observe = ,",
             "interval = 5s\ninterval = 6s",  // duplicate
             "intervall = 5s",                // typo
             "interval",                      // no '='
             "= 5s",
             "listen = 0.0.0.0:8080",          // network: not supported
             "port = 9000",
             "persist = /var/lib/osie",
             "remote = https://example.invalid",
             "history_capacity = 99999999999999999"}) {
        EXPECT_FALSE(parse_agent_config(bad).ok()) << bad;
    }
    const auto e = parse_agent_config("listen = 127.0.0.1:1");
    ASSERT_FALSE(e.ok());
    EXPECT_NE(e.error().message.find("no network listener"), std::string::npos);
    EXPECT_FALSE(parse_agent_config(std::string(70 * 1024, '#')).ok()) << "oversized config";
}

TEST(AgentConfig, FileSafetyChecks) {
    EXPECT_EQ(load_agent_config("/nonexistent/osie/agent.conf").error().code, Errc::not_found);
    EXPECT_TRUE(load_agent_config(write_tmp("ok.conf", "interval = 10s\n", 0600)).ok());
    EXPECT_EQ(load_agent_config(write_tmp("ww.conf", "interval = 10s\n", 0666)).error().code, Errc::permission_denied)
        << "a world-writable config decides what is observed: refused";
    EXPECT_EQ(load_agent_config(write_tmp("gw.conf", "interval = 10s\n", 0620)).error().code, Errc::permission_denied);
    EXPECT_FALSE(load_agent_config(::testing::TempDir()).ok()) << "a directory is not a config file";
    EXPECT_FALSE(load_agent_config(write_tmp("big.conf", std::string(70 * 1024, '#'), 0600)).ok());
}

namespace {

/// A fresh private directory for one test.
std::string tmp_dir(const std::string& name, mode_t mode = 0700) {
    const std::string d = ::testing::TempDir() + "osie_agent_" + std::to_string(::getpid()) + "_" + name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    ::chmod(d.c_str(), mode);
    return d;
}

void put_file(const std::string& path, const std::string& body, mode_t mode = 0600) {
    std::ofstream(path) << body;
    ::chmod(path.c_str(), mode);
}

}  // namespace

TEST(AgentConfig, SymlinksAndDirectoriesAreValidatedNotJustTheFinalFile) {
    const std::string d = tmp_dir("links");
    put_file(d + "/real.conf", "interval = 10s\n");
    // A symlink we own, in a directory only we can write: followed.
    ASSERT_EQ(::symlink((d + "/real.conf").c_str(), (d + "/link.conf").c_str()), 0);
    auto ok = load_agent_config(d + "/link.conf");
    ASSERT_TRUE(ok.ok()) << ok.error().describe();
    EXPECT_EQ(ok->interval, Millis{10'000});
    // A symlink loop fails promptly.
    ASSERT_EQ(::symlink((d + "/loop2").c_str(), (d + "/loop1").c_str()), 0);
    ASSERT_EQ(::symlink((d + "/loop1").c_str(), (d + "/loop2").c_str()), 0);
    EXPECT_FALSE(load_agent_config(d + "/loop1").ok());
    // A symlink to a directory or a FIFO is not a config file (and a FIFO must not block the loader).
    ASSERT_EQ(::symlink(d.c_str(), (d + "/dir.conf").c_str()), 0);
    EXPECT_FALSE(load_agent_config(d + "/dir.conf").ok());
    ASSERT_EQ(::mkfifo((d + "/fifo").c_str(), 0600), 0);
    ASSERT_EQ(::symlink((d + "/fifo").c_str(), (d + "/fifo.conf").c_str()), 0);
    const auto t0 = Clock::now();
    EXPECT_FALSE(load_agent_config(d + "/fifo.conf").ok());
    EXPECT_FALSE(load_agent_config(d + "/fifo").ok());
    EXPECT_LT(std::chrono::duration<double>(Clock::now() - t0).count(), 2.0) << "opening a FIFO blocked";

    // A file in a directory that others can write: they could replace it.
    const std::string open_dir = tmp_dir("openwrite", 0777);
    ::chmod(open_dir.c_str(), 0777);  // umask-proof
    put_file(open_dir + "/agent.conf", "interval = 10s\n");
    auto r = load_agent_config(open_dir + "/agent.conf");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, Errc::permission_denied) << r.error().describe();
    // ... and a safe-looking symlink whose target sits in such a directory.
    ASSERT_EQ(::symlink((open_dir + "/agent.conf").c_str(), (d + "/via.conf").c_str()), 0);
    EXPECT_FALSE(load_agent_config(d + "/via.conf").ok()) << "the target's directory is checked too";
    // ... and a symlink placed in such a directory, pointing to a safe file.
    ASSERT_EQ(::symlink((d + "/real.conf").c_str(), (open_dir + "/planted.conf").c_str()), 0);
    EXPECT_FALSE(load_agent_config(open_dir + "/planted.conf").ok()) << "a symlink in a writable directory can be swapped";
    const std::string group_dir = tmp_dir("groupwrite", 0770);
    put_file(group_dir + "/agent.conf", "interval = 10s\n");
    EXPECT_FALSE(load_agent_config(group_dir + "/agent.conf").ok());

    // A sticky, world-writable directory (like /tmp) protects our own entries from others.
    const std::string sticky = tmp_dir("sticky", 01777);
    ::chmod(sticky.c_str(), 01777);
    put_file(sticky + "/agent.conf", "interval = 10s\n");
    auto st = load_agent_config(sticky + "/agent.conf");
    EXPECT_TRUE(st.ok()) << st.error().describe();
    std::filesystem::remove_all(d);
    std::filesystem::remove_all(open_dir);
    std::filesystem::remove_all(group_dir);
    std::filesystem::remove_all(sticky);
}

TEST(AgentConfig, FilesOwnedByAnotherUserAreRefused) {
    if (::geteuid() != 0) GTEST_SKIP() << "needs root to create a file owned by another user";
    const std::string d = tmp_dir("owner");
    put_file(d + "/agent.conf", "interval = 10s\n", 0644);
    ASSERT_EQ(::chown((d + "/agent.conf").c_str(), 65534, 65534), 0);
    auto r = load_agent_config(d + "/agent.conf");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, Errc::permission_denied) << r.error().describe();
    ASSERT_EQ(::chown((d + "/agent.conf").c_str(), 0, 0), 0);
    ASSERT_EQ(::chown(d.c_str(), 65534, 65534), 0);  // a directory another user controls
    EXPECT_FALSE(load_agent_config(d + "/agent.conf").ok());
    std::filesystem::remove_all(d);
}

TEST(AgentConfig, ReplacingTheFileDuringLoadNeverYieldsUnsafeContent) {
    // A writer swaps the config path between a safe file and a world-writable
    // one (rename and symlink retargeting are both atomic). Every successful
    // load must come from the safe file: validation and reading are on the
    // same open file descriptor, not on the path twice.
    const std::string d = tmp_dir("swap");
    put_file(d + "/safe", "interval = 10s\n", 0600);
    put_file(d + "/unsafe", "interval = 20s\n", 0666);
    ::chmod((d + "/unsafe").c_str(), 0666);
    for (const bool use_symlink : {false, true}) {
        const std::string path = d + (use_symlink ? "/link.conf" : "/agent.conf");
        std::atomic<bool> done{false};
        std::thread swapper([&] {
            for (int i = 0; !done; ++i) {
                const std::string tmp = path + ".new";
                const std::string src = d + (i % 2 ? "/unsafe" : "/safe");
                if (use_symlink) {
                    (void)::unlink(tmp.c_str());
                    (void)::symlink(src.c_str(), tmp.c_str());
                } else {
                    std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing);
                    ::chmod(tmp.c_str(), i % 2 ? 0666 : 0600);
                }
                (void)std::rename(tmp.c_str(), path.c_str());
            }
        });
        int loaded = 0, unsafe = 0;
        const auto until = Clock::now() + std::chrono::milliseconds(1500);
        while (Clock::now() < until) {
            if (auto c = load_agent_config(path)) {
                ++loaded;
                if (c->interval != Millis{10'000}) ++unsafe;
            }
        }
        done = true;
        swapper.join();
        EXPECT_EQ(unsafe, 0) << (use_symlink ? "symlink" : "rename") << ": loaded the world-writable file's content "
                             << unsafe << " of " << loaded << " times";
        EXPECT_GT(loaded, 0);
    }
    std::filesystem::remove_all(d);
}

TEST(AgentConfig, RelativePathsAndOversizedReadsAreHandled) {
    const std::string d = tmp_dir("rel");
    put_file(d + "/agent.conf", "interval = 10s\n");
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(d);
    auto r = load_agent_config("agent.conf");
    std::filesystem::current_path(cwd);
    EXPECT_TRUE(r.ok()) << r.error().describe();
    // The 64 KiB limit is enforced on the bytes read, not only on st_size.
    put_file(d + "/big.conf", std::string(64 * 1024 + 1, '#'));
    EXPECT_FALSE(load_agent_config(d + "/big.conf").ok());
    put_file(d + "/edge.conf", std::string(64 * 1024, '#'));
    EXPECT_TRUE(load_agent_config(d + "/edge.conf").ok());
    std::filesystem::remove_all(d);
}

// ---------------------------------------------------------------- lifecycle ---

TEST(AgentRuntime, StartsCollectsAndStopsCleanly) {
    AgentRuntime rt(linux_os(), fast_cfg());
    EXPECT_FALSE(rt.snapshot());
    ASSERT_TRUE(rt.start().ok());
    EXPECT_FALSE(rt.start().ok()) << "double start is refused";
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < until && (!rt.snapshot() || rt.snapshot()->seq < 5)) std::this_thread::sleep_for(Millis{5});
    ASSERT_TRUE(rt.snapshot());
    EXPECT_GE(rt.snapshot()->seq, 5U);
    EXPECT_TRUE(rt.running());
    EXPECT_EQ(rt.health().state, AgentState::running);
    rt.stop();
    rt.stop();  // idempotent
    EXPECT_FALSE(rt.running());
    EXPECT_EQ(rt.health().state, AgentState::stopped);
    const auto seq = rt.snapshot()->seq;
    std::this_thread::sleep_for(Millis{60});
    EXPECT_EQ(rt.snapshot()->seq, seq) << "nothing is collected after stop";
}

TEST(AgentRuntime, RepeatedRestartsArePredictable) {
    AgentRuntime rt(linux_os(), fast_cfg());
    std::uint64_t last = 0;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(rt.start().ok()) << i;
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < until && (!rt.snapshot() || rt.snapshot()->seq <= last)) std::this_thread::sleep_for(Millis{2});
        ASSERT_GT(rt.snapshot()->seq, last) << "each start collects promptly";
        last = rt.snapshot()->seq;
        rt.stop();
        EXPECT_FALSE(rt.running());
    }
    EXPECT_LE(rt.health().history_points, 16U);
}

TEST(AgentRuntime, StopDuringActiveObservationIsPromptAndPublishesNothingPartial) {
    // A 3 s sampling window: stop() must interrupt it, not wait for it.
    auto c = fast_cfg(Millis{10'000}, Millis{3000});
    AgentRuntime rt(linux_os(), c);
    ASSERT_TRUE(rt.start().ok());
    std::this_thread::sleep_for(Millis{200});  // the first collection is now inside its window
    const auto t0 = Clock::now();
    rt.stop();
    const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    EXPECT_LT(ms, 500.0) << "shutdown waited for the sampling window";
    EXPECT_FALSE(rt.snapshot()) << "an interrupted collection is not published";
    EXPECT_EQ(rt.health().collections, 0U);
}

TEST(AgentRuntime, CollectionsNeverOverlap) {
    auto c = fast_cfg(Millis{10'000}, Millis{400});
    AgentRuntime rt(linux_os(), c);
    std::atomic<bool> first{false};
    std::thread t([&] { first = rt.collect_now(); });
    std::this_thread::sleep_for(Millis{100});
    EXPECT_FALSE(rt.collect_now()) << "a second collection while one runs is refused";
    t.join();
    EXPECT_TRUE(first);
    EXPECT_EQ(rt.health().collections, 1U);
    EXPECT_EQ(rt.health().rejected_manual, 1U);
}

TEST(AgentRuntime, OverrunningCollectionsSkipTicksInsteadOfQueueing) {
    // Window 60 ms but interval 20 ms: each collection overruns two ticks.
    auto c = fast_cfg(Millis{20}, Millis{60});
    AgentRuntime rt(linux_os(), c);
    ASSERT_TRUE(rt.start().ok());
    std::this_thread::sleep_for(Millis{700});
    rt.stop();
    const auto h = rt.health();
    EXPECT_GT(h.skipped_ticks, 0U);
    EXPECT_LE(h.collections, 12U) << "no backlog of collections is run back to back";
}

// ------------------------------------------------------- failure handling ---

TEST(AgentRuntime, MissingPressureMetricsAreUnsupportedNotFailures) {
    auto os = linux_os();
    ObservationSet sup;
    for (auto k : {ObservationKind::system_info, ObservationKind::cpu_topology, ObservationKind::cpu_times,
                   ObservationKind::load_average, ObservationKind::process_list, ObservationKind::memory,
                   ObservationKind::swap, ObservationKind::cgroup})
        sup.add(k);
    os->set_supported(sup);  // kernel without PSI
    AgentRuntime rt(std::move(os), fast_cfg());
    for (int i = 0; i < 5; ++i) rt.collect_now();
    const auto h = rt.health();
    const auto* psi = kind(h, ObservationKind::pressure_stall);
    ASSERT_NE(psi, nullptr);
    EXPECT_FALSE(psi->supported);
    EXPECT_EQ(psi->failed, 0U);
    EXPECT_EQ(h.state, AgentState::stopped) << "not started; state is only about the scheduler";
    EXPECT_TRUE(h.degraded_reasons.empty()) << "an unsupported metric does not degrade the agent";
}

TEST(AgentRuntime, CounterResetIsAFailureThenRecovers) {
    auto os = linux_os();
    // Counters go backwards (reset), repeatedly, then behave again.
    os->cpu_script = {MockOs::cpu(500, 500), MockOs::cpu(10, 10), MockOs::cpu(500, 500), MockOs::cpu(10, 10),
                      MockOs::cpu(500, 500), MockOs::cpu(10, 10)};
    for (std::uint64_t i = 1; i < 2000; ++i) os->cpu_script.push_back(MockOs::cpu(20 * i, 30 * i));
    auto c = fast_cfg();
    c.failure_threshold = 3;
    AgentRuntime rt(std::move(os), c);
    ASSERT_TRUE(rt.start().ok());
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < until && rt.health().collections < 3) std::this_thread::sleep_for(Millis{2});
    auto h = rt.health();
    const auto* cpu = kind(h, ObservationKind::cpu_times);
    ASSERT_NE(cpu, nullptr);
    EXPECT_GE(cpu->failed, 3U);
    EXPECT_NE(cpu->last_error.find("backwards"), std::string::npos) << cpu->last_error;
    EXPECT_EQ(h.state, AgentState::degraded);
    ASSERT_FALSE(h.degraded_reasons.empty());
    while (Clock::now() < until && kind(rt.health(), ObservationKind::cpu_times)->consecutive_failures > 0)
        std::this_thread::sleep_for(Millis{2});
    h = rt.health();
    rt.stop();
    EXPECT_EQ(kind(h, ObservationKind::cpu_times)->consecutive_failures, 0U);
    EXPECT_EQ(h.state, AgentState::running) << "recovers once counters behave";
}

TEST(AgentRuntime, TransientCollectorFailuresAreCountedAndSurvived) {
    auto os = linux_os();
    os->process_list_error = Errc::permission_denied;
    AgentRuntime rt(std::move(os), fast_cfg());
    for (int i = 0; i < 4; ++i) rt.collect_now();
    const auto h = rt.health();
    const auto* p = kind(h, ObservationKind::process_list);
    EXPECT_EQ(p->failed, 4U);
    EXPECT_NE(p->last_error.find("permission"), std::string::npos) << p->last_error;
    EXPECT_FALSE(h.degraded_reasons.empty());
    EXPECT_EQ(h.collections, 4U) << "the other observations are still collected";
    EXPECT_TRUE(rt.snapshot()->state->cpu.has_value());
}

TEST(AgentRuntime, ProcessDisappearingMidSampleAndPidReuseKeepIdentity) {
    auto os = linux_os();
    auto a = snap({proc(1, 0, "init", 0, 1 << 20), proc(40, 1, "web", 0, 50 << 20, 1000)});
    auto b = snap({proc(1, 0, "init", 0, 1 << 20)});  // pid 40 exited during the window
    b.vanished_count = 1;
    auto c1 = snap({proc(1, 0, "init", 0, 1 << 20), proc(40, 1, "cron", 0, 9 << 20, 999'000)});  // pid 40 reused
    os->proc_script = {a, b, c1};
    AgentRuntime rt(std::move(os), fast_cfg());
    rt.collect_now();  // a -> b: web vanished
    auto s = rt.snapshot();
    ASSERT_TRUE(s && s->state->processes);
    EXPECT_EQ(s->state->find_pid(40), nullptr);
    rt.collect_now();  // c1 -> c1: pid 40 is now a different process
    const auto hist = rt.history(10);
    ASSERT_EQ(hist.size(), 2U);
    std::set<std::pair<Pid, std::uint64_t>> ids;
    for (const auto& p : hist)
        for (const auto& q : p.processes)
            if (q.pid == 40) ids.insert({q.pid, q.start_time_us.value_or(0)});
    for (const auto& [pid, start] : ids) EXPECT_NE(start, 1040U) << "the exited web process is not attributed later";
    const auto* found = rt.snapshot()->state->find_pid(40);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->info.name, "cron");
    EXPECT_EQ(*found->info.start_time_us, 999'040U) << "(pid, start time) identifies the new process";
}

TEST(AgentRuntime, CpuQuotaChangeWhileRunningIsReflected) {
    auto os = linux_os();
    auto p = proc(40, 1, "web", 0, 50 << 20);
    p.cgroup = "/app";
    auto pp = p;
    pp.cpu_user_ns = 5'000'000;
    os->proc_script = {snap({p}), snap({pp})};
    CgroupInfo cg;
    cg.path = "/app";
    cg.cpu_quota_us = 100000;
    cg.cpu_period_us = 100000;
    cg.nr_periods = 10;
    cg.usage_usec = 1;
    os->cgroups = {cg};
    auto* raw = os.get();
    AgentRuntime rt(std::move(os), fast_cfg());
    rt.collect_now();
    EXPECT_EQ(*rt.snapshot()->state->cgroups.at(0).info.cpu_quota_us, 100000);
    {
        std::lock_guard lk(raw->mu);
        raw->cgroups[0].cpu_quota_us = 50000;  // `docker update --cpus 0.5` while the agent runs
    }
    rt.collect_now();
    EXPECT_EQ(*rt.snapshot()->state->cgroups.at(0).info.cpu_quota_us, 50000);
}

// ------------------------------------------------------------------ bounds ---

TEST(AgentRuntime, HistoryAndPerCollectionStateStayBoundedUnderProlongedSampling) {
    auto os = linux_os();
    std::vector<ProcessInfo> many;
    for (Pid i = 1; i <= 300; ++i) {
        auto p = proc(i, 1, "p" + std::to_string(i), static_cast<std::uint64_t>(i) * 1000, static_cast<std::uint64_t>(i) << 20);
        p.cgroup = "/cg" + std::to_string(i % 50);
        many.push_back(p);
    }
    os->proc_script = {snap(many)};
    AgentRuntime rt(std::move(os), fast_cfg());
    std::size_t bytes_at_capacity = 0;
    for (int i = 0; i < 400; ++i) {
        rt.collect_now();
        if (i == 20) bytes_at_capacity = rt.health().history_bytes;
    }
    const auto h = rt.health();
    EXPECT_EQ(h.history_points, 16U);
    EXPECT_LE(h.history_bytes, bytes_at_capacity * 11 / 10) << "memory stops growing once the history is full";
    for (const auto& p : rt.history(100)) EXPECT_LE(p.processes.size(), 4U) << "at most 2 x top_k processes per point";
    EXPECT_LE(rt.snapshot()->state->cgroups.size(), 2U) << "cgroups per collection bounded by cgroup_top_n";
    EXPECT_LE(rt.audit().recent(100000).size(), 256U) << "the audit ring is bounded";
}

// ------------------------------------------------------------- concurrency ---

TEST(AgentRuntime, ConcurrentReadersSeeConsistentSnapshots) {
    AgentRuntime rt(linux_os(), fast_cfg(Millis{5}, Millis{1}));
    ASSERT_TRUE(rt.start().ok());
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            std::uint64_t last_seq = 0;
            while (!stop) {
                if (auto s = rt.snapshot()) {
                    EXPECT_GE(s->seq, last_seq) << "snapshots never go back in time";
                    last_seq = s->seq;
                    EXPECT_TRUE(s->state != nullptr);
                    if (s->state->processes) EXPECT_FALSE(s->state->processes->value.empty());
                }
                const auto h = rt.health();
                EXPECT_LE(h.history_points, 16U);
                (void)rt.history(8);
                (void)to_json(h);
                ++reads;
            }
        });
    }
    std::this_thread::sleep_for(Millis{800});
    // Reconfigure while readers and the scheduler run.
    auto c = fast_cfg(Millis{5}, Millis{1});
    c.history_capacity = 8;
    rt.reconfigure(c);
    std::this_thread::sleep_for(Millis{300});
    stop = true;
    for (auto& t : readers) t.join();
    rt.stop();
    // Progress only: sanitizer builds are 10-100x slower.
    EXPECT_GT(reads.load(), 50U) << "readers were starved";
    EXPECT_LE(rt.health().history_points, 8U);
}

// --------------------------------------------------------------- read-only ---

TEST(AgentRuntime, ReadOnlyAndAudited) {
    AgentRuntime rt(linux_os(), fast_cfg());
    for (int i = 0; i < 3; ++i) rt.collect_now();
    for (const auto& d : rt.audit().recent(1000)) EXPECT_EQ(d.capability.rfind("observe.", 0), 0U) << d.capability;
    const auto caps = capabilities_json(rt);
    // Every act.* capability is listed as not allowed.
    std::size_t pos = 0, acts = 0;
    while ((pos = caps.find("\"id\":\"act.", pos)) != std::string::npos) {
        const auto end = caps.find('}', pos);
        EXPECT_NE(caps.substr(pos, end - pos).find("\"allowed\":false"), std::string::npos) << caps.substr(pos, end - pos);
        ++acts;
        pos = end;
    }
    EXPECT_GE(acts, 11U);
}

TEST(AgentRuntime, HealthJsonIsWellFormed) {
    AgentRuntime rt(linux_os(), fast_cfg());
    rt.collect_now();
    const auto j = to_json(rt.health());
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    for (const char* key : {"\"state\"", "\"collections\"", "\"skipped_ticks\"", "\"start_jitter\"", "\"observations\"",
                            "\"history_points\"", "\"degraded_reasons\"", "\"last_collection_age_s\""})
        EXPECT_NE(j.find(key), std::string::npos) << key;
}
