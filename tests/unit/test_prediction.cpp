// SPDX-License-Identifier: Apache-2.0
// CPU world model: feature encoding, model-file validation, bounded
// predictions, abstention, and read-only engine integration.
#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <sstream>

#include "osie/engine/engine.hpp"
#include "osie/platform/cpu_list.hpp"
#include "osie/prediction/world_model.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::predict;
using osie::test::MockOs;

namespace {

CgroupInfo cg(std::uint64_t usage_us, std::uint64_t periods, std::uint64_t throttled, std::uint64_t throttled_us,
              std::optional<std::int64_t> quota_us = 100000, std::uint32_t cpus = 4) {
    CgroupInfo c;
    c.path = "/test";
    c.usage_usec = usage_us;
    c.nr_periods = periods;
    c.nr_throttled = throttled;
    c.throttled_usec = throttled_us;
    c.cpu_quota_us = quota_us;
    c.cpu_period_us = 100000;
    c.cpuset_cpus = cpus;
    c.pids_current = 5;
    PressureStall p;
    p.resource = PsiResource::cpu;
    p.some.total_us = usage_us / 2;
    c.pressure.push_back(p);
    return c;
}

CgroupWindow window(double usage, double throttled, double quota = 1.0, double cpus = 4) {
    CgroupWindow w;
    w.secs = 0.5;
    w.usage_cores = usage;
    w.throttled_ratio = throttled > 0 ? 1.0 : 0.0;
    w.throttled_frac = throttled;
    w.psi_some = throttled > 0 ? 0.5 : 0.0;
    w.psi_full = 0.0;
    w.quota_cores = quota;
    w.cpuset_cpus = cpus;
    w.pids = 5;
    w.host_busy_frac = 0.2;
    w.host_cpus = 12;
    return w;
}

/// A hand-written linear model: usage = m_mech_usage_after, throttled =
/// m_excess_after * cap (approximately), ratio = 0, psi = 0.
std::string tiny_model(const std::string& extra = "", const std::string& actions = "action cpu_quota 0.5 3\naction none 0 0\n") {
    std::ostringstream o;
    o << "OSIEWM1\nid test-model\nkind linear\nhistory 2\nwindow_s 0.5\n"
      << "features m_mech_usage_after m_demand_after m_cap_after\n"
      << "mean 0 0 0\nscale 1 1 1\n"
      << "w2 1 0 0  0 1 -1  0 0 0  0 0 0\nb2 0 0 0 0\n"
      << "target usage_cores cores identity 0 cap\ntarget throttled_cores cores identity 0 64\n"
      << "target throttled_ratio fraction identity 0 1\ntarget psi_some fraction identity 0 1\n"
      << "conformal usage_cores 0.8 0.1\nconformal usage_cores 0.95 0.2\n"
      << "conformal throttled_cores 0.8 0.1\nconformal throttled_cores 0.95 0.3\n"
      << "conformal throttled_ratio 0.8 0.1\nconformal psi_some 0.8 0.1\n"
      << "ood_lo -1 -1 -1\nood_hi 10 10 10\nood_score_max 50\n"
      << actions << "provenance unit-test model\n" << extra << "end\n";
    return o.str();
}

/// The same model as an OSIEWM2 artifact with a support envelope.
std::string tiny_model_v2(const std::string& support = "support_arch x86_64 aarch64\nsupport_kernel 6.1 6.12\n",
                          const std::string& extra = "") {
    std::string t = tiny_model(support + "feature_schema 1\nmodel_version t1\ndata_version d1\n"
                                         "calibration_version c1\nsupport_cgroup 2\n"
                                         "support_throttle_semantics summed_per_cpu\nsupport_capacity 0.5 3\n"
                                         "workload_coverage arith hash\nsafe_throttle_max 0.25\n" + extra);
    t.replace(0, 7, "OSIEWM2");
    return t;
}

}  // namespace

TEST(CpuList, ParsesKernelFormat) {
    EXPECT_EQ(parse_cpu_list_count("0-3\n"), 4U);
    EXPECT_EQ(parse_cpu_list_count("0-3,8,10-11"), 7U);
    EXPECT_EQ(parse_cpu_list_count("5"), 1U);
    for (const char* bad : {"", "3-1", "a-b", "0-", "1,,2", "0-99999"}) EXPECT_FALSE(parse_cpu_list_count(bad)) << bad;
}

TEST(WorldFeatures, WindowsFromCounters) {
    auto w = make_window(cg(0, 0, 0, 0), cg(500'000, 5, 5, 1'000'000), 0.5);
    ASSERT_TRUE(w.ok());
    EXPECT_NEAR(w->usage_cores, 1.0, 1e-9);
    EXPECT_NEAR(*w->throttled_ratio, 1.0, 1e-9);
    EXPECT_NEAR(*w->throttled_frac, 2.0, 1e-9) << "throttled time is summed over CPUs and must not be clamped to 1";
    EXPECT_NEAR(*w->quota_cores, 1.0, 1e-9);
    EXPECT_NEAR(*w->psi_some, 0.5, 1e-9);
    EXPECT_NEAR(demand_estimate(*w), 3.0, 1e-9) << "usage + throttled time";

    auto unlimited = make_window(cg(0, 0, 0, 0, std::nullopt), cg(100'000, 0, 0, 0, std::nullopt), 0.5);
    ASSERT_TRUE(unlimited.ok());
    EXPECT_FALSE(unlimited->quota_cores);
    EXPECT_EQ(*unlimited->throttled_ratio, 0.0) << "no periods elapsed: nothing could be throttled";

    CgroupInfo no_ctrl = cg(0, 0, 0, 0);
    no_ctrl.nr_periods.reset();
    no_ctrl.nr_throttled.reset();
    no_ctrl.throttled_usec.reset();
    auto w2 = make_window(no_ctrl, no_ctrl, 0.5);
    ASSERT_TRUE(w2.ok());
    EXPECT_FALSE(w2->throttled_ratio) << "missing statistics stay missing, never 0";

    EXPECT_FALSE(make_window(cg(500, 0, 0, 0), cg(100, 0, 0, 0), 0.5).ok()) << "counter went backwards";
    EXPECT_FALSE(make_window(cg(0, 0, 0, 0), cg(1, 0, 0, 0), 0.0).ok());
}

TEST(WorldFeatures, EncodingIsCompleteAndValidated) {
    std::vector<CgroupWindow> h{window(1.0, 2.0), window(1.0, 2.0)};
    auto e = encode(h, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(e.ok()) << e.error().message;
    ASSERT_EQ(e->values.size(), feature_names().size());
    auto f = [&](const std::string& n) {
        const auto& names = feature_names();
        return e->values[static_cast<std::size_t>(std::find(names.begin(), names.end(), n) - names.begin())];
    };
    EXPECT_NEAR(f("m_cap_after"), 2.0, 1e-9);
    EXPECT_NEAR(f("m_demand_after"), 3.0, 1e-9);
    EXPECT_NEAR(f("m_mech_usage_after"), 2.0, 1e-9);
    EXPECT_TRUE(e->missing.empty());

    const auto& w = h.back();
    for (auto bad : {Intervention{ActionKind::cpu_quota, 1, -1}, Intervention{ActionKind::cpu_quota, 1, 0},
                     Intervention{ActionKind::cpu_quota, 1, 99}, Intervention{ActionKind::cpuset, 4, 0},
                     Intervention{ActionKind::cpuset, 4, 2.5}, Intervention{ActionKind::offered_load, 1, 20},
                     Intervention{ActionKind::cpu_quota, 1, std::nan("")}}) {
        EXPECT_FALSE(validate(bad, w).ok()) << to_string(bad.kind) << " " << bad.proposed;
        EXPECT_FALSE(encode(h, bad).ok());
    }
    EXPECT_FALSE(encode({}, Intervention{}).ok()) << "no history";

    auto missing = h;
    missing[0].throttled_ratio.reset();
    missing[1].psi_some.reset();
    auto em = encode(missing, Intervention{});
    ASSERT_TRUE(em.ok());
    EXPECT_EQ(em->missing.size(), 2U);
}

TEST(WorldModelFile, LoadsAndRejectsMalformedFiles) {
    auto ok = WorldModel::load_from_memory(tiny_model());
    ASSERT_TRUE(ok.ok()) << ok.error().message;
    EXPECT_EQ((*ok)->parameter_count(), 16U);
    EXPECT_EQ((*ok)->provenance(), "unit-test model");
    const std::string good = tiny_model();
    auto replace = [&](const std::string& from, const std::string& to) {
        std::string s = good;
        const auto at = s.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        return s.replace(at, from.size(), to);
    };
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"OSIEWM1", "OSIEWM9"},
        {"end\n", ""},  // truncated
        {"m_cap_after\n", "m_not_a_feature\n"},  // schema mismatch
        {"w2 1 0 0", "w2 1 0"},  // shape mismatch
        {"b2 0 0 0 0", "b2 0 0 0 nan"},
        {"b2 0 0 0 0", "b2 0 0 0 1e300"},
        {"scale 1 1 1", "scale 1 0 1"},
        {"kind linear", "kind linear\nkind mlp"},  // duplicate key
        {"provenance unit-test model", "provenance caf\xc3\xa9"},
        {"conformal psi_some 0.8 0.1\n", ""},  // a target without an interval
        {"conformal psi_some 0.8 0.1", "conformal psi_some 1.5 0.1"},
        {"action cpu_quota 0.5 3\naction none 0 0\n", ""},  // no supported actions
        {"history 2", "history 0"},
        {"target psi_some fraction identity 0 1", "target psi_some fraction exp 0 1"},
    };
    for (const auto& [from, to] : cases)
        EXPECT_FALSE(WorldModel::load_from_memory(replace(from, to)).ok()) << "accepted: " << to;
    EXPECT_FALSE(WorldModel::load("/nonexistent.osiewm").ok());
}

TEST(WorldModelPredict, BoundedIntervalsAndProvenance) {
    auto m = WorldModel::load_from_memory(tiny_model());
    ASSERT_TRUE(m.ok());
    std::vector<CgroupWindow> h{window(1.0, 2.0), window(1.0, 2.0)};
    auto p = (*m)->predict(h, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(p.ok()) << p.error().message;
    EXPECT_FALSE(p->abstained);
    ASSERT_EQ(p->targets.size(), 4U);
    EXPECT_NEAR(p->targets[0].expected, 2.0, 1e-9);  // usage = min(demand 3, cap 2)
    EXPECT_NEAR(p->targets[0].current, 1.0, 1e-9);
    EXPECT_NEAR(p->targets[1].expected, 1.0, 1e-9);  // throttled = demand - cap
    for (const auto& t : p->targets)
        for (const auto& iv : t.intervals) {
            EXPECT_LE(iv.lo, t.expected);
            EXPECT_GE(iv.hi, t.expected);
            EXPECT_GE(iv.lo, 0.0) << "intervals respect physical bounds";
        }
    EXPECT_LE(p->targets[0].intervals.back().hi, 2.0 + 1e-9) << "usage interval capped at the new quota";
    EXPECT_EQ(p->model_id, "test-model");
}

TEST(WorldModelPredict, RawOutputAboveCapacityIsClampedAndFlagged) {
    // usage weight 2 on demand: raw output 6 cores with a 2-core quota.
    auto m = WorldModel::load_from_memory(
        tiny_model().replace(tiny_model().find("w2 1 0 0"), 8, "w2 0 2 0"));
    ASSERT_TRUE(m.ok()) << m.error().message;
    auto p = (*m)->predict({window(1, 2), window(1, 2)}, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p->targets[0].clamped);
    EXPECT_NEAR(p->targets[0].expected, 2.0, 1e-9);
}

TEST(WorldModelPredict, AbstainsOutsideValidatedScope) {
    auto m = WorldModel::load_from_memory(tiny_model());
    ASSERT_TRUE(m.ok());
    std::vector<CgroupWindow> h{window(1.0, 2.0), window(1.0, 2.0)};
    // Valid intervention, but outside the quota range the model was trained on.
    auto far = (*m)->predict(h, Intervention{ActionKind::cpu_quota, 1.0, 8.0});
    ASSERT_TRUE(far.ok());
    EXPECT_TRUE(far->abstained);
    EXPECT_TRUE(far->out_of_distribution);
    // Less history than the model was trained with.
    auto short_h = (*m)->predict({window(1.0, 2.0)}, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(short_h.ok());
    EXPECT_TRUE(short_h->abstained);
    // Missing telemetry.
    auto miss = h;
    miss[1].throttled_frac.reset();
    miss[1].throttled_ratio.reset();
    auto pm = (*m)->predict(miss, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(pm.ok());
    EXPECT_TRUE(pm->abstained);
    // A feature far outside the training range.
    auto huge = h;
    for (auto& w : huge) w.usage_cores = 50, w.cpuset_cpus = 64, w.host_cpus = 64;
    auto ph = (*m)->predict(huge, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(ph.ok());
    EXPECT_TRUE(ph->abstained);
    // Interval too wide to be useful.
    auto wide = WorldModel::load_from_memory(tiny_model("abstain_width usage_cores 0.1\n"));
    ASSERT_TRUE(wide.ok());
    auto pw = (*wide)->predict(h, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(pw.ok());
    EXPECT_TRUE(pw->abstained);
    // An action the model was not trained for is an error, not a guess.
    EXPECT_FALSE((*m)->predict(h, Intervention{ActionKind::cpuset, 4, 2}).ok());
}

TEST(WorldModelFile, RandomMutationsNeverCrash) {
    const std::string base = tiny_model();
    std::uint64_t st = 0x2545F4914F6CDD1DULL;
    auto rnd = [&](std::uint64_t n) {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        return st % n;
    };
    std::vector<CgroupWindow> h{window(1.0, 2.0), window(1.0, 2.0)};
    for (int i = 0; i < 3000; ++i) {
        std::string s = base;
        for (std::uint64_t e = 0, n = 1 + rnd(3); e < n && !s.empty(); ++e) {
            switch (rnd(3)) {
                case 0: s[rnd(s.size())] = static_cast<char>('0' + rnd(10)); break;
                case 1: s.erase(rnd(s.size()), 1 + rnd(4)); break;
                default: s.insert(rnd(s.size() + 1), 1, " -.e9\n"[rnd(6)]); break;
            }
        }
        auto m = WorldModel::load_from_memory(s);
        if (!m) continue;
        auto p = (*m)->predict(h, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
        if (!p) continue;
        for (const auto& t : p->targets) ASSERT_TRUE(std::isfinite(t.expected));
    }
}

// ----------------------------------------------------------------- engine ---

namespace {

std::unique_ptr<MockOs> linux_with_cgroup(bool with_throttle_stats = true) {
    auto os = std::make_unique<MockOs>(Platform::linux_os);
    os->info.arch = "x86_64";
    os->info.kernel_release = "6.8.0-45-generic";
    for (std::uint64_t i = 0; i < 40; ++i) os->cpu_script.push_back(MockOs::cpu(100 * i, 1000 + 100 * i));
    ProcessSnapshot ps;
    auto p = osie::test::proc(42, 1, "api", 0, 100 << 20);
    p.cgroup = "/system.slice/api.service";
    ps.processes = {p};
    os->proc_script = {ps};
    std::uint64_t u = 0, t = 0, n = 0;
    for (int i = 0; i < 10; ++i) {
        auto c = cg(u, n, n, t);
        if (!with_throttle_stats) {
            c.nr_periods.reset();
            c.nr_throttled.reset();
            c.throttled_usec.reset();
        }
        os->cgroup_script.push_back(c);
        // 1 core used and 2 CPUs' worth of throttled time per 0.05 s window
        // (the tests' window length), consistent with summed-per-CPU accounting.
        u += 50'000;
        t += 100'000;
        n += 5;
    }
    return os;
}

EngineConfig fast(bool experimental = true) {
    EngineConfig c;
    c.sample_interval = Millis{5};
    c.experimental_prediction = experimental;
    return c;
}

std::shared_ptr<const IWorldModel> model_with_window(double secs, bool v2 = true) {
    std::string text = v2 ? tiny_model_v2() : tiny_model();
    text.replace(text.find("window_s 0.5"), 12, "window_s " + std::to_string(secs));
    auto m = WorldModel::load_from_memory(text);
    EXPECT_TRUE(m.ok());
    return *m;
}

}  // namespace

TEST(EnginePredict, AnswersReadOnlyAndAudits) {
    auto os = linux_with_cgroup();
    Engine e(std::move(os), std::make_unique<intent::RuleParser>(), fast());
    e.set_world_model(model_with_window(0.05));
    PredictTarget t;
    t.pid = 42;
    auto a = e.predict(t, ActionKind::cpu_quota, 2.0);
    EXPECT_TRUE(a.answered) << a.text;
    EXPECT_EQ(a.cgroup, "/system.slice/api.service");
    EXPECT_NE(a.text.find("No changes were applied."), std::string::npos);
    EXPECT_NE(a.text.find("Not predicted: application latency"), std::string::npos);
    EXPECT_NE(a.text.find("-AFFECTS-> CPU bandwidth throttling (invariant"), std::string::npos)
        << "mechanisms come from the architecture graph with their epistemic status\n" << a.text;
    ASSERT_TRUE(a.prediction);
    bool predicted = false;
    for (const auto& d : e.audit().recent(1000)) {
        if (d.capability == "observe.predict.cpu_world_model") predicted = d.allowed;
        EXPECT_NE(d.capability.rfind("act.", 0), 0U) << "prediction must never touch an action capability";
    }
    EXPECT_TRUE(predicted);
}

TEST(EnginePredict, RefusesOrAbstainsWhenEvidenceIsMissing) {
    {  // macOS: no cgroup v2 CPU bandwidth control
        Engine e(std::make_unique<MockOs>(Platform::macos), std::make_unique<intent::RuleParser>(), fast());
        e.set_world_model(model_with_window(0.05));
        auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpu_quota, 2.0);
        EXPECT_FALSE(a.answered);
        EXPECT_NE(a.text.find("macOS"), std::string::npos);
    }
    {  // no model installed
        Engine e(linux_with_cgroup(), std::make_unique<intent::RuleParser>(), fast());
        auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpu_quota, 2.0);
        EXPECT_FALSE(a.answered);
        EXPECT_NE(a.text.find("No CPU world model"), std::string::npos);
    }
    {  // invalid intervention
        Engine e(linux_with_cgroup(), std::make_unique<intent::RuleParser>(), fast());
        e.set_world_model(model_with_window(0.05));
        auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpu_quota, -1.0);
        EXPECT_FALSE(a.answered);
        EXPECT_NE(a.text.find("Cannot evaluate"), std::string::npos) << a.text;
    }
    {  // cpu controller statistics unavailable -> abstain, no numbers
        Engine e(linux_with_cgroup(false), std::make_unique<intent::RuleParser>(), fast());
        e.set_world_model(model_with_window(0.05));
        auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpu_quota, 2.0);
        EXPECT_FALSE(a.answered);
        EXPECT_NE(a.text.find("Prediction unavailable"), std::string::npos) << a.text;
        EXPECT_EQ(a.text.find("expected"), std::string::npos) << "no numbers when abstaining";
    }
    {  // unknown process
        Engine e(linux_with_cgroup(), std::make_unique<intent::RuleParser>(), fast());
        e.set_world_model(model_with_window(0.05));
        auto a = e.predict(PredictTarget{std::nullopt, "nosuchprocess", ""}, ActionKind::cpu_quota, 2.0);
        EXPECT_FALSE(a.answered);
    }
    {  // action the model does not support
        Engine e(linux_with_cgroup(), std::make_unique<intent::RuleParser>(), fast());
        e.set_world_model(model_with_window(0.05));
        auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpuset, 2.0);
        EXPECT_FALSE(a.answered);
        EXPECT_NE(a.text.find("not trained"), std::string::npos);
    }
}

TEST(EnginePredict, CapabilityIsObserveOnly) {
    const auto reg = CapabilityRegistry::builtin();
    const auto* c = reg.find("observe.predict.cpu_world_model");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->kind, CapabilityKind::observe);
    EXPECT_FALSE(c->has_side_effects);
    EXPECT_TRUE(validate(*c).empty());
}

TEST(WorldModelPredict, ResidualOnMechanisticPrior) {
    // All-zero weights: the prediction is exactly the mechanistic prior.
    std::string text = tiny_model("residual_on mechanistic\n");
    text.replace(text.find("w2 1 0 0  0 1 -1"), 16, "w2 0 0 0  0 0  0");
    auto m = WorldModel::load_from_memory(text);
    ASSERT_TRUE(m.ok()) << m.error().message;
    auto p = (*m)->predict({window(1.0, 2.0), window(1.0, 2.0)}, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(p.ok());
    EXPECT_NEAR(p->targets[0].expected, 2.0, 1e-9);  // usage = min(demand 3, quota 2)
    EXPECT_NEAR(p->targets[1].expected, 1.0, 1e-9);  // throttled = 3 - 2
    EXPECT_NEAR(p->targets[2].expected, 1.0, 1e-9);  // the quota still binds
    EXPECT_NEAR(p->targets[3].expected, 0.5, 1e-9);  // PSI unchanged
    auto relaxed = (*m)->predict({window(1.0, 2.0), window(1.0, 2.0)}, Intervention{ActionKind::cpu_quota, 1.0, 3.0});
    ASSERT_TRUE(relaxed.ok());
    EXPECT_NEAR(relaxed->targets[1].expected, 0.0, 1e-9) << "quota 3 >= demand 3: no throttling";
}

TEST(WorldModelShipped, LoadsAndGetsTheBatchCaseRight) {
    const std::string path = std::string(OSIE_SOURCE_DIR) + "/models/world_cpu.osiewm";
    auto m = WorldModel::load(path);
    if (!m && m.error().code == Errc::not_found) GTEST_SKIP() << "no world model in models/";
    ASSERT_TRUE(m.ok()) << m.error().message;
    EXPECT_TRUE((*m)->supports(ActionKind::cpu_quota));
    EXPECT_FALSE((*m)->provenance().empty());
    // Regression for the finding in docs/WORLD_MODEL_RESULTS.md: 3 always-runnable
    // threads under a 1-core quota (usage 1, throttled 2 cores). With a 2-core
    // quota the kernel gives usage ~2 and throttling ~1; the validation-selected
    // learned residual predicted usage 1.08 here.
    std::vector<CgroupWindow> h;
    for (int i = 0; i < 6; ++i) {
        auto w = window(1.0, 2.0, 1.0, 4);
        w.pids = 7;
        w.psi_some = 0.66;
        w.host_busy_frac = 0.3;
        h.push_back(w);
    }
    auto p = (*m)->predict(h, Intervention{ActionKind::cpu_quota, 1.0, 2.0});
    ASSERT_TRUE(p.ok()) << p.error().message;
    EXPECT_NEAR(p->targets[0].expected, 2.0, 0.05) << "usage";
    EXPECT_NEAR(p->targets[1].expected, 1.0, 0.05) << "throttled cores";
    for (const auto& t : p->targets) EXPECT_TRUE(std::isfinite(t.expected));
}

// ---------------------------------------------------------------- v0.2 ----

TEST(WorldModelV2, LoaderRequiresTheSupportEnvelope) {
    ASSERT_TRUE(WorldModel::load_from_memory(tiny_model_v2()).ok());
    // Every support line is required in OSIEWM2.
    for (const char* key : {"support_arch", "support_kernel", "feature_schema", "model_version", "data_version",
                            "calibration_version", "support_cgroup", "support_throttle_semantics", "support_capacity",
                            "workload_coverage"}) {
        std::string t = tiny_model_v2();
        const auto pos = t.find(std::string(key) + " ");
        ASSERT_NE(pos, std::string::npos) << key;
        t.erase(pos, t.find('\n', pos) - pos + 1);
        EXPECT_FALSE(WorldModel::load_from_memory(t).ok()) << "missing " << key;
    }
    // v2-only keys are rejected in a v1 file, unknown feature schema in v2.
    EXPECT_FALSE(WorldModel::load_from_memory(tiny_model("support_cgroup 2\n")).ok());
    std::string t = tiny_model_v2();
    t.replace(t.find("feature_schema 1"), 16, "feature_schema 9");
    EXPECT_FALSE(WorldModel::load_from_memory(t).ok());
    // Duplicates of non-repeatable keys are rejected.
    EXPECT_FALSE(WorldModel::load_from_memory(tiny_model_v2("support_arch x86_64\nsupport_arch aarch64\nsupport_kernel 6.1 6.12\n")).ok());
    EXPECT_FALSE(WorldModel::load_from_memory(tiny_model_v2("support_arch x86_64\nsupport_kernel 6.12 6.1\n")).ok())
        << "inverted kernel range";
}

TEST(WorldModelV2, KernelVersionParsing) {
    EXPECT_EQ(*kernel_version("6.10.14-linuxkit"), std::make_pair(6, 10));
    EXPECT_EQ(*kernel_version("5.15.0-1063-azure"), std::make_pair(5, 15));
    EXPECT_EQ(*kernel_version("6.8"), std::make_pair(6, 8));
    for (const char* bad : {"", "linux", "6", "6.", ".8", "999999.1"}) EXPECT_FALSE(kernel_version(bad)) << bad;
}

TEST(WorldModelV2, OutsideTheEnvelopeIsWithheld) {
    auto m = *WorldModel::load_from_memory(tiny_model_v2());
    const std::vector<CgroupWindow> h = {window(1.0, 0.0, 2.0), window(1.0, 0.0, 2.0)};
    const Intervention a{ActionKind::cpu_quota, 2.0, 1.5};
    auto ok = m->predict_in(h, a, RuntimeEnvironment{"x86_64", "6.8.0-45-generic", 2});
    ASSERT_TRUE(ok.ok());
    EXPECT_FALSE(ok->abstained) << (ok->reasons.empty() ? "" : ok->reasons.front());
    for (const auto& env : {RuntimeEnvironment{"riscv64", "6.8.0", 2}, RuntimeEnvironment{"x86_64", "5.15.0-azure", 2},
                            RuntimeEnvironment{"x86_64", "6.18.1", 2}, RuntimeEnvironment{"x86_64", "6.8.0", 1},
                            RuntimeEnvironment{"x86_64", "garbage", 2}}) {
        auto p = m->predict_in(h, a, env);
        ASSERT_TRUE(p.ok());
        EXPECT_TRUE(p->abstained) << env.arch << " " << env.kernel_release;
        EXPECT_FALSE(p->environment_issues.empty());
        EXPECT_EQ(p->throttling_assessment, "unknown");
    }
    // An OSIEWM1 model has no envelope: its validity here is unknown.
    auto legacy = *WorldModel::load_from_memory(tiny_model());
    auto p = legacy->predict_in(h, a, RuntimeEnvironment{"x86_64", "6.8.0", 2});
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p->abstained);
}

TEST(WorldModelV2, InconsistentThrottlingCountersAreWithheld) {
    auto m = *WorldModel::load_from_memory(tiny_model_v2());
    auto w = window(1.0, 0.0, 2.0);
    w.throttled_ratio = 0.0;
    w.throttled_frac = 0.8;  // throttled time without any throttled period
    auto p = m->predict({w, w}, Intervention{ActionKind::cpu_quota, 2.0, 1.5});
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p->abstained);
    auto w2 = window(1.0, 9.0, 2.0);  // more throttled CPU-seconds per second than CPUs
    w2.cpuset_cpus = 4;
    p = m->predict({w2, w2}, Intervention{ActionKind::cpu_quota, 2.0, 1.5});
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p->abstained);
}

TEST(WorldModelV2, ThrottlingAssessmentUsesTheUpperBoundNotThePointEstimate) {
    auto m = *WorldModel::load_from_memory(tiny_model_v2());
    // Demand 1 core, quota to 2: no throttling predicted, 95% upper bound 0.3 > 0.25 -> "possible", not "low".
    auto p = m->predict({window(1.0, 0.0, 1.5), window(1.0, 0.0, 1.5)}, Intervention{ActionKind::cpu_quota, 1.5, 2.0});
    ASSERT_TRUE(p.ok());
    ASSERT_FALSE(p->abstained);
    EXPECT_EQ(p->targets[1].expected, 0.0);
    EXPECT_EQ(p->throttling_assessment, "possible") << "a point estimate of zero is not enough to call it low";
    // With a tight interval the same case is "low"; a heavy cut is "likely".
    std::string t = tiny_model_v2();
    t.replace(t.find("conformal throttled_cores 0.95 0.3"), 34, "conformal throttled_cores 0.95 0.1");
    auto tight = *WorldModel::load_from_memory(t);
    p = tight->predict({window(1.0, 0.0, 1.5), window(1.0, 0.0, 1.5)}, Intervention{ActionKind::cpu_quota, 1.5, 2.0});
    EXPECT_EQ(p->throttling_assessment, "low");
    p = tight->predict({window(2.0, 1.0, 3.0), window(2.0, 1.0, 3.0)}, Intervention{ActionKind::cpu_quota, 3.0, 1.0});
    EXPECT_EQ(p->throttling_assessment, "likely");
}

TEST(WorldModelV2, ResidualGateFallsBackToMechanismOutOfDistribution) {
    // A residual model whose correction is +0.5 cores on usage for every input.
    std::ostringstream o;
    o << "features m_mech_usage_after\nmean 0\nscale 1\nw2 0 0 0 0\nb2 0.5 0 0 0\n";
    std::string base = tiny_model_v2("support_arch x86_64\nsupport_kernel 6.1 6.12\n", "residual_on mechanistic\nresidual_gate usage_cores 1.0\n");
    base.replace(base.find("features"), base.find("target usage_cores") - base.find("features"), o.str());
    base.replace(base.find("ood_lo -1 -1 -1\nood_hi 10 10 10"), 30, "ood_lo -1\nood_hi 10");
    auto m = WorldModel::load_from_memory(base);
    ASSERT_TRUE(m.ok()) << m.error().message;
    const std::vector<CgroupWindow> h = {window(1.0, 0.0, 2.0), window(1.0, 0.0, 2.0)};
    auto in = (*m)->predict(h, Intervention{ActionKind::cpu_quota, 2.0, 2.5});
    ASSERT_TRUE(in.ok());
    EXPECT_NEAR(in->targets[0].expected, 1.5, 1e-9) << "in-distribution: mechanism (1.0) + residual (0.5)";
    EXPECT_TRUE(in->targets[0].residual_applied);
    // Gate smaller than the correction -> mechanism only.
    std::string strict = base;
    strict.replace(strict.find("residual_gate usage_cores 1.0"), 29, "residual_gate usage_cores 0.1");
    auto m2 = *WorldModel::load_from_memory(strict);
    auto out = m2->predict(h, Intervention{ActionKind::cpu_quota, 2.0, 2.5});
    EXPECT_NEAR(out->targets[0].expected, 1.0, 1e-9);
    EXPECT_FALSE(out->targets[0].residual_applied);
}

TEST(EnginePredict, IsOffUnlessExplicitlyEnabled) {
    Engine e(linux_with_cgroup(), std::make_unique<intent::RuleParser>(), fast(false));
    e.set_world_model(model_with_window(0.05));
    auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpu_quota, 2.0);
    EXPECT_FALSE(a.answered);
    EXPECT_NE(a.text.find("experimental"), std::string::npos);
    for (const auto& d : e.audit().recent(100)) EXPECT_NE(d.capability, "observe.cgroup.v2") << "nothing observed";
}

TEST(EnginePredict, LegacyModelIsWithheldInTheEngine) {
    Engine e(linux_with_cgroup(), std::make_unique<intent::RuleParser>(), fast());
    e.set_world_model(model_with_window(0.05, /*v2=*/false));
    auto a = e.predict(PredictTarget{42, "", ""}, ActionKind::cpu_quota, 2.0);
    EXPECT_FALSE(a.answered);
    EXPECT_NE(a.text.find("predates compatibility metadata"), std::string::npos) << a.text;
}

// Property: for random windows and interventions, a prediction is either
// withheld or has ordered, physically bounded intervals; identical inputs
// give identical outputs.
TEST(WorldModelV2, PropertyBoundsAndDeterminism) {
    auto m = *WorldModel::load_from_memory(tiny_model_v2());
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0.0, 4.0);
    int answered = 0;
    for (int i = 0; i < 3000; ++i) {
        const double q = 0.5 + u(rng) * 0.6;
        auto w = window(u(rng), u(rng) * 0.5, q);
        const Intervention a{ActionKind::cpu_quota, q, 0.5 + u(rng) * 0.6};
        auto p1 = m->predict({w, w}, a);
        auto p2 = m->predict({w, w}, a);
        if (!p1.ok()) continue;
        ASSERT_TRUE(p2.ok());
        if (p1->abstained) continue;
        ++answered;
        for (std::size_t t = 0; t < p1->targets.size(); ++t) {
            const auto& x = p1->targets[t];
            EXPECT_EQ(x.expected, p2->targets[t].expected);
            EXPECT_GE(x.expected, 0.0);
            for (const auto& iv : x.intervals) {
                EXPECT_LE(iv.lo, x.expected + 1e-12);
                EXPECT_GE(iv.hi, x.expected - 1e-12);
            }
            if (x.name == "usage_cores") EXPECT_LE(x.expected, std::min(a.proposed, 4.0) + 1e-9);
        }
    }
    EXPECT_GT(answered, 100);
}
