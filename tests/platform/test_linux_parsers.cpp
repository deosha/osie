// SPDX-License-Identifier: Apache-2.0
// Linux adapter parsers + a fixture /proc tree (deterministic, no live kernel).
#if defined(__linux__)
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <fstream>

#include "platform/linux/linux_os.hpp"

using namespace osie;
using namespace osie::linux_os;

TEST(LinuxParse, ProcStatWithHostileComm) {
    // comm may contain spaces and ')' - the parser must use the *last* ')'.
    const std::string s =
        "4242 (my (weird) proc) S 1 4242 4242 0 -1 4194560 500 0 7 0 250 50 0 0 20 5 3 0 12345 104857600 2560 "
        "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0";
    auto st = parse_proc_stat(s);
    ASSERT_TRUE(st);
    EXPECT_EQ(st->comm, "my (weird) proc");
    EXPECT_EQ(st->state, 'S');
    EXPECT_EQ(st->ppid, 1);
    EXPECT_EQ(st->majflt, 7U);
    EXPECT_EQ(st->utime_ticks, 250U);
    EXPECT_EQ(st->stime_ticks, 50U);
    EXPECT_EQ(st->nice, 5);
    EXPECT_EQ(st->threads, 3U);
    EXPECT_EQ(st->start_ticks, 12345U);
    EXPECT_EQ(st->vsize, 104857600U);
    EXPECT_EQ(st->rss_pages, 2560U);
    EXPECT_FALSE(parse_proc_stat("garbage"));
    EXPECT_FALSE(parse_proc_stat("1 (x) S 0"));
}

TEST(LinuxParse, Psi) {
    auto ps = parse_psi("some avg10=12.50 avg60=3.00 avg300=1.00 total=123456\n"
                        "full avg10=2.25 avg60=0.50 avg300=0.10 total=999\n",
                        PsiResource::memory);
    ASSERT_TRUE(ps);
    EXPECT_DOUBLE_EQ(ps->some.avg10, 12.5);
    EXPECT_EQ(ps->some.total_us, 123456U);
    ASSERT_TRUE(ps->full);
    EXPECT_DOUBLE_EQ(ps->full->avg10, 2.25);
    auto cpu_only_some = parse_psi("some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n", PsiResource::cpu);
    ASSERT_TRUE(cpu_only_some);
    EXPECT_FALSE(cpu_only_some->full) << "older kernels have no cpu 'full' line";
    EXPECT_FALSE(parse_psi("", PsiResource::io));
}

TEST(LinuxParse, CpuListAndFields) {
    EXPECT_EQ(parse_cpu_list("0-3,8,10-11\n"), (std::vector<unsigned>{0, 1, 2, 3, 8, 10, 11}));
    EXPECT_TRUE(parse_cpu_list("").empty());
    EXPECT_EQ(kv_field("MemTotal:       16384 kB\nMemFree: 1 kB\n", "MemTotal", true), 16384U * 1024);
    EXPECT_FALSE(kv_field("MemTotalX: 5 kB\n", "MemTotal", true));
    EXPECT_EQ(kv_field("Uid:\t1000\t1000\t1000\t1000\n", "Uid", false), 1000U);
    EXPECT_EQ(space_field("usage_usec 50\nnr_throttled 7\n", "nr_throttled"), 7U);
    EXPECT_FALSE(space_field("nr_throttled_x 7\n", "nr_throttled"));
}

namespace {
void put(const std::string& path, const std::string& body) {
    const auto slash = path.rfind('/');
    std::string dir = path.substr(0, slash);
    std::string cur;
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            cur = dir.substr(0, i);
            ::mkdir(cur.c_str(), 0755);
        }
    }
    std::ofstream(path) << body;
}
}  // namespace

TEST(LinuxFixture, CgroupThrottlingAndPsiFromFixtureTree) {
    const std::string root = ::testing::TempDir() + "osie_fixture_" + std::to_string(::getpid());
    put(root + "/proc/stat", "cpu  100 0 50 1000 5 0 0 0 0 0\ncpu0 100 0 50 1000 5 0 0 0 0 0\nbtime 1700000000\n"
                             "ctxt 42\nprocs_running 3\nprocs_blocked 1\n");
    put(root + "/proc/self/mountinfo", "30 1 0:26 / /sys/fs/cgroup rw,nosuid - cgroup2 cgroup2 rw\n");
    put(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory pids\n");
    put(root + "/proc/pressure/cpu", "some avg10=40.00 avg60=20.00 avg300=5.00 total=100\n");
    put(root + "/proc/pressure/memory", "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    put(root + "/proc/pressure/io", "some avg10=1.00 avg60=0.00 avg300=0.00 total=5\nfull avg10=0.50 avg60=0.00 avg300=0.00 total=2\n");
    put(root + "/sys/fs/cgroup/system.slice/web.service/cpu.stat",
        "usage_usec 9000000\nuser_usec 8000000\nsystem_usec 1000000\nnr_periods 1000\nnr_throttled 400\nthrottled_usec 5000000\n");
    put(root + "/sys/fs/cgroup/system.slice/web.service/cpu.max", "50000 100000\n");
    put(root + "/sys/fs/cgroup/system.slice/web.service/memory.current", "1048576\n");
    put(root + "/sys/fs/cgroup/system.slice/web.service/memory.max", "max\n");
    put(root + "/sys/fs/cgroup/system.slice/web.service/cpu.pressure", "some avg10=55.00 avg60=1 avg300=1 total=1\n");

    LinuxOperatingSystem os(root);
    EXPECT_TRUE(os.supported().contains(ObservationKind::pressure_stall));
    EXPECT_TRUE(os.supported().contains(ObservationKind::cgroup));
    auto cpu = os.cpu_sample();
    ASSERT_TRUE(cpu.ok()) << cpu.error().describe();
    EXPECT_EQ(cpu->value.aggregate.iowait, 5U);
    EXPECT_EQ(*cpu->value.runnable_tasks, 3U);
    EXPECT_EQ(*cpu->value.context_switches_total, 42U);
    EXPECT_EQ(cpu->value.per_cpu.size(), 1U);
    auto psi = os.pressure();
    ASSERT_TRUE(psi.ok());
    EXPECT_EQ(psi->value.size(), 3U);
    auto cg = os.cgroup("/system.slice/web.service");
    ASSERT_TRUE(cg.ok()) << cg.error().describe();
    EXPECT_EQ(*cg->value.cpu_quota_us, 50000);
    EXPECT_EQ(*cg->value.cpu_period_us, 100000U);
    EXPECT_EQ(*cg->value.nr_throttled, 400U);
    EXPECT_FALSE(cg->value.memory_max) << "'max' means unlimited -> no value";
    EXPECT_EQ(*cg->value.memory_current, 1048576U);
    ASSERT_EQ(cg->value.pressure.size(), 1U);
    EXPECT_EQ(cg->value.systemd_unit, "web.service");
    EXPECT_FALSE(os.cgroup("/../etc").ok()) << "path traversal must be rejected";
    EXPECT_EQ(os.cgroup("/missing.service").error().code, Errc::not_found);
}

TEST(LinuxFixture, NoPsiNoCgroup) {
    const std::string root = ::testing::TempDir() + "osie_fixture_bare_" + std::to_string(::getpid());
    put(root + "/proc/stat", "cpu  1 0 1 10 0 0 0 0 0 0\nbtime 1\n");
    LinuxOperatingSystem os(root);
    EXPECT_FALSE(os.supported().contains(ObservationKind::pressure_stall));
    EXPECT_FALSE(os.supported().contains(ObservationKind::cgroup));
    EXPECT_EQ(os.pressure().error().code, Errc::unsupported);
    EXPECT_EQ(os.cgroup("/").error().code, Errc::unsupported);
}
#endif
