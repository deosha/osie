// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <thread>

#include "osie/capabilities/capability.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using osie::test::MockOs;

TEST(Capabilities, BuiltinRegistryIsValid) {
    auto reg = CapabilityRegistry::builtin();
    ASSERT_FALSE(reg.all().empty());
    for (const auto& c : reg.all()) EXPECT_TRUE(validate(c).empty()) << c.id;
    // Every observation kind has exactly one capability.
    for (unsigned k = 0; k < static_cast<unsigned>(ObservationKind::count_); ++k) {
        const auto kind = static_cast<ObservationKind>(k);
        if (kind == ObservationKind::swap || kind == ObservationKind::process_detail) continue;
        EXPECT_NE(reg.for_observation(kind), nullptr) << to_string(kind);
    }
}

TEST(Capabilities, ValidationRejectsInconsistentDescriptors) {
    CapabilityDescriptor d;
    d.id = "observe.bad";
    d.description = "x";
    d.kind = CapabilityKind::observe;
    d.platforms = PlatformSet::all();
    d.has_side_effects = true;  // invalid for observe
    EXPECT_FALSE(validate(d).empty());

    CapabilityDescriptor a;
    a.id = "act.thing";
    a.description = "x";
    a.kind = CapabilityKind::act;
    a.platforms = PlatformSet::all();
    a.risk = RiskLevel::high;
    a.has_side_effects = true;
    a.side_effects = "y";
    a.requires_confirmation = false;  // invalid for high risk
    a.preconditions = a.postconditions = "z";
    auto v = validate(a);
    EXPECT_FALSE(v.empty());

    CapabilityRegistry r;
    EXPECT_FALSE(r.add(a).ok());
    CapabilityDescriptor bad_id = d;
    bad_id.id = "Observe Bad";
    EXPECT_FALSE(r.add(bad_id).ok());
}

TEST(Capabilities, DuplicateIdsRejected) {
    auto reg = CapabilityRegistry::builtin();
    auto dup = *reg.find("observe.cpu.times");
    EXPECT_FALSE(reg.add(dup).ok());
}

TEST(Policy, AllActionsDeniedInV1) {
    auto reg = CapabilityRegistry::builtin();
    SafetyPolicy pol;  // default config
    MockOs os(Platform::linux_os);
    std::size_t acts = 0;
    for (const auto& c : reg.all()) {
        if (c.kind != CapabilityKind::act) continue;
        ++acts;
        for (auto p : {Platform::macos, Platform::linux_os}) {
            auto d = pol.authorize(c, p, os.supported());
            EXPECT_FALSE(d.allowed) << c.id;
            EXPECT_NE(d.reason.find("read-only"), std::string::npos);
        }
        EXPECT_FALSE(c.implemented) << "V1 must not ship an executor for " << c.id;
    }
    EXPECT_GE(acts, 6U);
    // Even a permissive config cannot enable actions: read-only is enforced by kind.
    SafetyPolicy permissive(PolicyConfig{false, true});
    EXPECT_FALSE(permissive.authorize(*reg.find("act.process.terminate"), Platform::macos, os.supported()).allowed);
}

TEST(Policy, PlatformAndKernelSupportChecked) {
    auto reg = CapabilityRegistry::builtin();
    SafetyPolicy pol;
    MockOs mac(Platform::macos);
    MockOs lin(Platform::linux_os);
    EXPECT_TRUE(pol.authorize(*reg.find("observe.cpu.times"), Platform::macos, mac.supported()).allowed);
    EXPECT_FALSE(pol.authorize(*reg.find("observe.pressure.psi"), Platform::macos, mac.supported()).allowed);
    EXPECT_TRUE(pol.authorize(*reg.find("observe.pressure.psi"), Platform::linux_os, lin.supported()).allowed);
    // PSI compiled out / disabled on this kernel:
    ObservationSet no_psi;
    no_psi.add(ObservationKind::cpu_times);
    auto d = pol.authorize(*reg.find("observe.pressure.psi"), Platform::linux_os, no_psi);
    EXPECT_FALSE(d.allowed);
    EXPECT_NE(d.reason.find("unsupported"), std::string::npos);
}

TEST(Broker, AuditsEveryDecision) {
    MockOs os(Platform::macos);
    os.cpu_script = {MockOs::cpu(1, 1)};
    auto reg = CapabilityRegistry::builtin();
    SafetyPolicy pol;
    AuditLog audit;
    ObservationBroker b(os, reg, pol, audit);
    EXPECT_TRUE(b.cpu_sample().ok());
    auto r = b.pressure();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, Errc::unsupported);
    auto rec = audit.recent(10);
    ASSERT_EQ(rec.size(), 2U);
    EXPECT_TRUE(rec[0].allowed);
    EXPECT_EQ(rec[0].capability, "observe.cpu.times");
    EXPECT_FALSE(rec[1].allowed);
    EXPECT_EQ(audit.denied_count(), 1U);
}

TEST(AuditLog, WritesJsonLinesAndIsThreadSafe) {
    const std::string path = ::testing::TempDir() + "osie_audit_test.jsonl";
    std::remove(path.c_str());
    AuditLog log(64);
    ASSERT_TRUE(log.open_file(path).ok());
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&log, t] {
            for (int i = 0; i < 100; ++i)
                log.record(PolicyDecision{"observe.x", (i % 2) == 0, "t" + std::to_string(t), std::chrono::system_clock::now()});
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(log.recent(1000).size(), 64U);  // bounded ring
    EXPECT_EQ(log.denied_count(), 200U);
    std::ifstream in(path);
    std::string line;
    std::size_t lines = 0;
    while (std::getline(in, line)) {
        ASSERT_EQ(line.front(), '{');
        ASSERT_EQ(line.back(), '}');
        ++lines;
    }
    EXPECT_EQ(lines, 400U);
}
