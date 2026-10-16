// SPDX-License-Identifier: Apache-2.0
// osie-agent runtime: the lifecycle and ownership contract under concurrency
// (stop, destruction, collect_now and reconfigure racing each other), high
// process counts, process churn and prolonged running. CI also runs these
// under AddressSanitizer and ThreadSanitizer.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

#include "osie/agent/runtime.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::agent;
using osie::test::MockOs;
using osie::test::proc;
using Clock = std::chrono::steady_clock;

namespace {

#if defined(OSIE_SANITIZED)
constexpr int kScale = 4;  // sanitizer builds are 5-50x slower: fewer iterations, same assertions
#else
constexpr int kScale = 1;
#endif

AgentConfig cfg(Millis interval, Millis window, std::size_t capacity = 16, std::size_t top_k = 2) {
    AgentConfig c;  // constructed directly: tests may use intervals below the config-file minimum
    c.interval = interval;
    c.sample_window = window;
    c.history_capacity = capacity;
    c.history_top_k = top_k;
    c.cgroup_top_n = 2;
    return c;
}

ProcessSnapshot snap(std::vector<ProcessInfo> v) {
    ProcessSnapshot s;
    s.processes = std::move(v);
    return s;
}

/// A mock with monotonically increasing CPU counters and a fixed process list
/// of `n` processes (each with a distinct CPU time and memory size).
std::unique_ptr<MockOs> os_with(std::size_t n = 2) {
    auto os = std::make_unique<MockOs>(Platform::linux_os);
    os->cpu_gen = [](int i) { return MockOs::cpu(100ULL * static_cast<std::uint64_t>(i), 1000ULL * static_cast<std::uint64_t>(i)); };
    MemoryMetrics m;
    m.total_bytes = 8ULL << 30;
    m.available_bytes = 4ULL << 30;
    os->mem_script = {m};
    PressureStall psi;
    psi.resource = PsiResource::cpu;
    os->psi = {psi};
    std::vector<ProcessInfo> v;
    for (std::size_t i = 1; i <= n; ++i)
        v.push_back(proc(static_cast<Pid>(i), 1, "p" + std::to_string(i), i * 1000, i << 20));
    os->proc_script = {snap(std::move(v))};
    return os;
}

/// Blocks inside processes() until the collection is cancelled (or 10 s pass).
void block_until_cancelled(MockOs& os, std::atomic<bool>& entered) {
    os.on_processes = [&entered](const CancellationToken& c) {
        entered = true;
        const auto until = Clock::now() + std::chrono::seconds(10);
        while (!c.cancelled() && Clock::now() < until) std::this_thread::sleep_for(Millis{1});
    };
}

void wait_for(const std::atomic<bool>& flag) {
    const auto until = Clock::now() + std::chrono::seconds(10);
    while (!flag && Clock::now() < until) std::this_thread::sleep_for(Millis{1});
}

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

}  // namespace

// ------------------------------------------------------ lifecycle contract ---

TEST(AgentLifecycle, StopCancelsAndAwaitsAManualCollection) {
    // collect_now() on another thread, stuck inside an observation. stop()
    // must cancel it and return only once it has finished.
    auto os = os_with();
    std::atomic<bool> entered{false}, finished{false}, result{true};
    block_until_cancelled(*os, entered);
    AgentRuntime rt(std::move(os), cfg(Millis{10'000}, Millis{5}));
    std::thread t([&] {
        result = rt.collect_now();
        finished = true;
    });
    wait_for(entered);
    const auto t0 = Clock::now();
    rt.stop();
    const double ms = ms_since(t0);
    EXPECT_TRUE(finished) << "stop() returned while a collection was still running";
    EXPECT_LT(ms, 2000.0);
    t.join();
    EXPECT_FALSE(result) << "a cancelled collection reports that nothing was published";
    EXPECT_FALSE(rt.snapshot());
    EXPECT_EQ(rt.health().collections, 0U);
}

// Destroying the runtime while another thread is inside a member call is
// undefined behaviour (the lifetime contract in runtime.hpp), so no test does
// it. These two tests use the two orderings the contract allows.

TEST(AgentLifecycle, StopThenJoinThenDestroyEndsAStuckCallerPromptly) {
    // Another thread is stuck inside collect_now(). The owner calls stop()
    // (allowed concurrently with any member call), joins that thread, and
    // only then destroys the runtime.
    auto os = os_with();
    std::atomic<bool> entered{false}, result{true};
    block_until_cancelled(*os, entered);
    auto rt = std::make_unique<AgentRuntime>(std::move(os), cfg(Millis{10'000}, Millis{5}));
    std::thread caller([&rt, &result] { result = rt->collect_now(); });
    wait_for(entered);
    const auto t0 = Clock::now();
    rt->stop();
    caller.join();
    EXPECT_LT(ms_since(t0), 2000.0) << "stop() did not cancel the stuck collection";
    EXPECT_FALSE(result) << "a cancelled collection reports that nothing was published";
    rt.reset();  // no other thread can be inside a member call now
}

TEST(AgentLifecycle, SharedOwnershipDefersDestructionToTheLastCaller) {
    // Callers each hold a shared_ptr for as long as they make calls. The
    // owner may drop its reference at any time; the runtime is destroyed by
    // whichever thread drops the last one, after every call has returned.
    auto rt = std::make_shared<AgentRuntime>(os_with(8), cfg(Millis{2}, Millis{1}));
    ASSERT_TRUE(rt->start().ok());
    const std::weak_ptr<AgentRuntime> watch = rt;
    std::atomic<bool> done{false};
    std::atomic<int> calls{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i)
        callers.emplace_back([self = rt, &done, &calls, i]() mutable {
            while (!done) {
                if (i % 2) (void)self->collect_now();
                else (void)to_json(self->health());
                (void)self->snapshot();
                ++calls;
            }
            self->stop();   // any caller may stop it
            self.reset();   // the last one to get here runs the destructor
        });
    std::this_thread::sleep_for(Millis{200});
    rt.reset();  // the owner lets go while the callers are mid-call
    EXPECT_FALSE(watch.expired()) << "destroyed while callers still held it";
    std::this_thread::sleep_for(Millis{100});
    EXPECT_FALSE(watch.expired());
    done = true;
    for (auto& t : callers) t.join();
    EXPECT_TRUE(watch.expired()) << "the last caller's release destroys the runtime";
    EXPECT_GT(calls.load(), 0);
}

// The scheduler thread is internal: destroying a started runtime from its
// owner (with no other callers) is always allowed, even mid-collection.
TEST(AgentLifecycle, DestructionWhileTheSchedulerIsMidCollection) {
    auto os = os_with();
    std::atomic<bool> entered{false};
    block_until_cancelled(*os, entered);
    auto rt = std::make_unique<AgentRuntime>(std::move(os), cfg(Millis{20}, Millis{5}));
    ASSERT_TRUE(rt->start().ok());
    wait_for(entered);
    const auto t0 = Clock::now();
    rt.reset();
    EXPECT_LT(ms_since(t0), 2000.0);
}

TEST(AgentLifecycle, NoCollectionStartsWhileStopIsInProgress) {
    // A collection is stuck until cancelled; stop() is called; while stop()
    // waits, further collect_now() calls are refused without touching the OS.
    auto os = os_with();
    auto* mock = os.get();
    std::atomic<bool> entered{false}, release{false};
    mock->on_processes = [&](const CancellationToken&) {
        entered = true;
        const auto until = Clock::now() + std::chrono::seconds(10);
        while (!release && Clock::now() < until) std::this_thread::sleep_for(Millis{1});
    };
    AgentRuntime rt(std::move(os), cfg(Millis{10'000}, Millis{5}));
    std::thread first([&] { (void)rt.collect_now(); });
    wait_for(entered);
    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        rt.stop();
        stopped = true;
    });
    std::this_thread::sleep_for(Millis{50});
    EXPECT_FALSE(stopped) << "stop() waits for the in-flight collection";
    auto os_calls = [&] {
        std::lock_guard lk(mock->mu);
        return mock->process_calls + mock->in_processes.load();
    };
    const int calls_before = os_calls();
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(rt.collect_now());
    EXPECT_EQ(os_calls(), calls_before) << "a refused collection must not reach the OS";
    release = true;
    first.join();
    stopper.join();
    EXPECT_TRUE(stopped);
    EXPECT_GE(rt.health().rejected_manual, 20U);
    // After stop() has returned the runtime is usable again (e.g. --once, restart).
    mock->on_processes = nullptr;
    EXPECT_TRUE(rt.collect_now());
    ASSERT_TRUE(rt.start().ok());
    rt.stop();
}

TEST(AgentLifecycle, SnapshotsOutliveTheRuntime) {
    std::shared_ptr<const AgentSnapshot> s;
    {
        AgentRuntime rt(os_with(4), cfg(Millis{10'000}, Millis{2}));
        ASSERT_TRUE(rt.collect_now());
        s = rt.snapshot();
    }
    ASSERT_TRUE(s && s->state && s->state->processes);
    EXPECT_EQ(s->state->processes->value.size(), 4U) << "a snapshot owns its data; it does not borrow from the runtime";
}

TEST(AgentLifecycle, StateIsStoppedAfterStopEvenWithRacingManualCollections) {
    // Before the fix, a manual collection finishing just after stop() set
    // "stopping" could overwrite the state with "running".
    AgentRuntime rt(os_with(), cfg(Millis{2}, Millis{1}));
    std::atomic<bool> done{false};
    std::vector<std::thread> hammer;
    for (int i = 0; i < 3; ++i)
        hammer.emplace_back([&] {
            while (!done) (void)rt.collect_now();
        });
    for (int i = 0; i < 200 / kScale; ++i) {
        ASSERT_TRUE(rt.start().ok());
        std::this_thread::sleep_for(Millis{1});
        rt.stop();
        EXPECT_FALSE(rt.running()) << "iteration " << i;
        // Manual collections continue after stop(); none may revive the state.
        std::this_thread::sleep_for(Millis{1});
        EXPECT_EQ(rt.health().state, AgentState::stopped) << "iteration " << i;
    }
    done = true;
    for (auto& t : hammer) t.join();
}

// ------------------------------------------------------------ concurrency ---

TEST(AgentConcurrency, EverythingAtOnceKeepsTheInvariants) {
    // Scheduler restarts, manual collections, reconfiguration and readers,
    // all concurrently. Invariants: collections never overlap; history and
    // per-point process counts respect the configuration in force; the
    // state is "stopped" after the final stop().
    auto os = os_with(40);
    auto* mock = os.get();
    AgentRuntime rt(std::move(os), cfg(Millis{3}, Millis{1}, 32, 4));
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0}, manual{0}, reconfigs{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            std::uint64_t last = 0;
            while (!done) {
                if (auto s = rt.snapshot()) {
                    EXPECT_GE(s->seq, last);
                    last = s->seq;
                    ASSERT_TRUE(s->state);
                }
                const auto h = rt.health();
                EXPECT_LE(h.history_points, 32U);
                for (const auto& p : rt.history(64)) EXPECT_LE(p.processes.size(), 8U);
                (void)to_json(h);
                (void)rt.config();
                ++reads;
            }
        });
    for (int i = 0; i < 2; ++i)
        ts.emplace_back([&] {
            while (!done)
                if (rt.collect_now()) ++manual;
        });
    ts.emplace_back([&] {
        for (std::size_t i = 0; !done; ++i) {
            rt.reconfigure(cfg(Millis{3}, Millis{1}, i % 2 ? 8 : 32, i % 2 ? 1 : 4));
            ++reconfigs;
            std::this_thread::sleep_for(Millis{2});
        }
    });
    ts.emplace_back([&] {
        while (!done) {
            if (rt.start().ok()) {
                std::this_thread::sleep_for(Millis{5});
                rt.stop();
            }
        }
    });
    std::this_thread::sleep_for(Millis{1500});
    done = true;
    for (auto& t : ts) t.join();
    rt.stop();
    EXPECT_EQ(mock->max_in_processes.load(), 1) << "two collections overlapped";
    EXPECT_FALSE(rt.running());
    EXPECT_EQ(rt.health().state, AgentState::stopped);
    EXPECT_GT(reads.load(), 20U);
    EXPECT_GT(manual.load(), 0U);
    EXPECT_GT(reconfigs.load(), 10U);
}

TEST(AgentConcurrency, ConcurrentReconfigurationsLeaveConfigAndHistoryConsistent) {
    // Two reconfigurations racing must not leave the configuration from one
    // and the history bounds from the other.
    AgentRuntime rt(os_with(40), cfg(Millis{10'000}, Millis{1}, 16, 4));
    for (int round = 0; round < 50 / kScale; ++round) {
        std::thread a([&] {
            for (int i = 0; i < 20; ++i) rt.reconfigure(cfg(Millis{10'000}, Millis{1}, 8, 1));
        });
        std::thread b([&] {
            for (int i = 0; i < 20; ++i) rt.reconfigure(cfg(Millis{10'000}, Millis{1}, 24, 3));
        });
        a.join();
        b.join();
        const auto c = rt.config();
        for (int i = 0; i < 30; ++i) ASSERT_TRUE(rt.collect_now());
        const auto h = rt.health();
        ASSERT_EQ(h.history_points, c.history_capacity) << "history bound does not match the configuration in force";
        for (const auto& p : rt.history(100)) ASSERT_LE(p.processes.size(), 2 * c.history_top_k);
    }
}

// --------------------------------------------- reload enforces the bounds ---

TEST(AgentBounds, ReducingHistoryBoundsOnReloadTrimsExistingPoints) {
    AgentRuntime rt(os_with(300), cfg(Millis{10'000}, Millis{1}, 64, 16));
    for (int i = 0; i < 64; ++i) ASSERT_TRUE(rt.collect_now());
    const auto before = rt.health();
    ASSERT_EQ(before.history_points, 64U);
    for (const auto& p : rt.history(64)) ASSERT_GT(p.processes.size(), 2U);

    // Reload: fewer points and fewer processes per point.
    rt.reconfigure(cfg(Millis{10'000}, Millis{1}, 10, 1));
    const auto after = rt.health();
    EXPECT_EQ(after.history_points, 10U) << "capacity is enforced at reload, not at the next collection";
    const auto pts = rt.history(100);
    ASSERT_EQ(pts.size(), 10U);
    for (const auto& p : pts) {
        EXPECT_LE(p.processes.size(), 2U) << "existing points must respect the new top_k (2 x top_k processes)";
        // The kept processes are the top one by CPU and the top one by memory.
        const bool has_top_memory = std::any_of(p.processes.begin(), p.processes.end(),
                                                [](const ProcessPoint& q) { return q.pid == 300; });
        EXPECT_TRUE(has_top_memory) << "the largest process (p300) is kept";
    }
    EXPECT_LT(after.history_bytes, before.history_bytes / 20) << before.history_bytes << " -> " << after.history_bytes;
    // The bytes after the reload match what fresh collections under the new
    // configuration hold (no leftover capacity in the trimmed points).
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(rt.collect_now());
    EXPECT_LE(after.history_bytes, rt.health().history_bytes * 11 / 10);

    // Growing the bounds again keeps the trimmed points as they are.
    rt.reconfigure(cfg(Millis{10'000}, Millis{1}, 64, 16));
    EXPECT_EQ(rt.health().history_points, 10U);
}

// -------------------------------- process count, churn, prolonged running ---

TEST(AgentScale, HighProcessCountStaysWithinBounds) {
    // History memory depends on the configuration, not on how many processes exist.
    auto run = [](std::size_t n, double& worst_ms) {
        auto os = os_with(0);
        std::vector<ProcessInfo> many;
        many.reserve(n);
        for (std::size_t i = 1; i <= n; ++i) {
            auto p = proc(static_cast<Pid>(i), 1, "worker-" + std::to_string(i % 1000), (i * 7919) % 100'000 * 1000,
                          ((i * 104'729) % 5000 + 1) << 12);
            p.cgroup = "/svc/" + std::to_string(i % 500);
            many.push_back(std::move(p));
        }
        os->proc_script = {snap(std::move(many))};
        AgentRuntime rt(std::move(os), cfg(Millis{10'000}, Millis{1}, 32, 16));
        worst_ms = 0;
        for (int i = 0; i < 40; ++i) {
            EXPECT_TRUE(rt.collect_now());
            worst_ms = std::max(worst_ms, rt.health().last_collection_ms);
        }
        const auto h = rt.health();
        EXPECT_EQ(h.history_points, 32U);
        for (const auto& p : rt.history(100)) EXPECT_LE(p.processes.size(), 32U);
        EXPECT_EQ(rt.snapshot()->state->processes->value.size(), n);
        EXPECT_LE(rt.snapshot()->state->cgroups.size(), 2U);
        for (const auto& k : h.kinds) EXPECT_EQ(k.failed, 0U) << to_string(k.kind) << ": " << k.last_error;
        return h.history_bytes;
    };
    const std::size_t n = 20'000 / static_cast<std::size_t>(kScale);
    double worst_small = 0, worst_large = 0;
    const std::size_t small = run(200, worst_small);
    const std::size_t large = run(n, worst_large);
    EXPECT_LE(large, small * 11 / 10) << "history bytes grew with the process count: " << small << " -> " << large;
    std::printf("[ info ] 200 processes: worst collection %.1f ms; %zu processes: %.1f ms; history %zu / %zu bytes\n",
                worst_small, n, worst_large, small, large);
}

TEST(AgentScale, ProcessChurnKeepsIdentityAndBounds) {
    // Every process lives for a few collections; PIDs are reused from a
    // small pool with new start times, so PID reuse is constant.
    auto os = os_with(0);
    os->proc_gen = [](int call) {
        const int epoch = call / 2;  // the two lists of one collection share an epoch mostly
        std::vector<ProcessInfo> v;
        for (int slot = 0; slot < 400; ++slot) {
            const int gen = (epoch + slot) / 3;  // each slot's process is replaced every 3 epochs
            const auto pid = static_cast<Pid>(1000 + (slot * 7 + gen) % 500);  // reused PIDs
            const auto start = 10'000'000ULL * static_cast<std::uint64_t>(gen + 1) + static_cast<std::uint64_t>(slot);
            auto p = proc(pid, 1, "g" + std::to_string(gen) + "s" + std::to_string(slot),
                          static_cast<std::uint64_t>(call) * 1000 * static_cast<std::uint64_t>(slot % 17 + 1),
                          static_cast<std::uint64_t>(slot % 31 + 1) << 20, 0);
            p.start_time_us = start;
            v.push_back(std::move(p));
        }
        ProcessSnapshot s;
        s.processes = std::move(v);
        return s;
    };
    AgentRuntime rt(std::move(os), cfg(Millis{10'000}, Millis{1}, 64, 8));
    std::size_t bytes_full = 0;
    const int n = 2000 / kScale;
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(rt.collect_now());
        if (i == 100) bytes_full = rt.health().history_bytes;
    }
    const auto h = rt.health();
    EXPECT_EQ(h.history_points, 64U);
    EXPECT_LE(h.history_bytes, bytes_full * 12 / 10) << "memory is flat under churn";
    // (pid, start time) is one process: it never carries two names.
    std::map<std::pair<Pid, std::uint64_t>, std::string> names;
    for (const auto& p : rt.history(64))
        for (const auto& q : p.processes) {
            ASSERT_LE(p.processes.size(), 16U);
            const auto [it, fresh] = names.emplace(std::make_pair(q.pid, q.start_time_us.value_or(0)), q.name);
            if (!fresh) EXPECT_EQ(it->second, q.name) << "pid " << q.pid << " merged two processes";
        }
    for (const auto& k : h.kinds)
        if (k.kind == ObservationKind::process_list) EXPECT_EQ(k.failed, 0U) << k.last_error;
}

TEST(AgentScale, ProlongedSchedulerRunWithChurnIsBoundedAndHealthy) {
    // Thousands of scheduled collections with churn: memory, the audit ring,
    // the jitter ring and the history stay at their bounds; no failures.
    auto os = os_with(0);
    os->proc_gen = [](int call) {
        std::vector<ProcessInfo> v;
        for (int i = 0; i < 200; ++i) {
            auto p = proc(static_cast<Pid>(100 + (i + call / 10) % 300), 1, "c" + std::to_string(i),
                          static_cast<std::uint64_t>(call) * 500, static_cast<std::uint64_t>(i + 1) << 16, 0);
            p.start_time_us = static_cast<std::uint64_t>(call / 10) * 1'000'000ULL + static_cast<std::uint64_t>(i);
            v.push_back(std::move(p));
        }
        ProcessSnapshot s;
        s.processes = std::move(v);
        return s;
    };
    // Sanitizer builds collect 10-50x slower: a smaller history still fills.
    const std::size_t capacity = kScale == 1 ? 128 : 8;
    AgentRuntime rt(std::move(os), cfg(Millis{1}, Millis{1}, capacity, 8));
    ASSERT_TRUE(rt.start().ok());
    std::size_t bytes_full = 0;
    const auto t0 = Clock::now();
    while (ms_since(t0) < 4000) {
        std::this_thread::sleep_for(Millis{50});
        const auto h = rt.health();
        if (bytes_full == 0 && h.history_points == capacity) bytes_full = h.history_bytes;
        if (bytes_full) EXPECT_LE(h.history_bytes, bytes_full * 12 / 10);
    }
    rt.stop();
    const auto h = rt.health();
    std::printf("[ info ] %llu collections, %llu skipped ticks, jitter p99 %.2f ms\n",
                static_cast<unsigned long long>(h.collections), static_cast<unsigned long long>(h.skipped_ticks),
                h.start_jitter.p99_ms);
    EXPECT_GT(h.collections, 2 * capacity);
    EXPECT_EQ(h.history_points, capacity);
    EXPECT_EQ(h.failed_collections, 0U) << to_json(h);
    EXPECT_LE(rt.audit().recent(1'000'000).size(), 256U);
    EXPECT_GE(h.start_jitter.n, h.collections) << "one jitter value per tick that ran";
    EXPECT_LE(h.start_jitter.n, h.collections + 1) << "(+1: the collection cancelled by stop())";
}
