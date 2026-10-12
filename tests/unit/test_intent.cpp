// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "osie/intent/intent.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::intent;

TEST(Tokenizer, SplitsAndNormalises) {
    auto t = tokenize("Why is Google Chrome's helper using 2.5GB?? pid=1234 com.apple.WebKit kernel_task -x-");
    std::vector<std::string> lower;
    for (const auto& x : t) lower.push_back(x.lower);
    std::vector<std::string> want = {"why", "is", "google", "chrome", "s", "helper", "using", "2.5gb", "pid", "1234",
                                     "com.apple.webkit", "kernel_task", "x"};
    EXPECT_EQ(lower, want);
    EXPECT_EQ(t[2].text, "Google");
    EXPECT_EQ(t[2].begin, 7U);
    EXPECT_TRUE(tokenize("").empty());
    EXPECT_TRUE(tokenize("?!  ..").empty());
}

struct Case {
    const char* text;
    Intent intent;
    TargetKind target;
    const char* value;
};

class RuleParserCases : public ::testing::TestWithParam<Case> {};

TEST_P(RuleParserCases, IntentAndTarget) {
    const auto& c = GetParam();
    RuleParser p;
    test::MockOs os(Platform::macos);
    auto reg = CapabilityRegistry::builtin();
    auto toks = tokenize(c.text);
    auto f = build_frame(p.parse(toks), toks, Platform::macos, os.supported(), reg);
    EXPECT_EQ(to_string(f.intent), to_string(c.intent)) << c.text;
    EXPECT_EQ(to_string(f.target.kind), to_string(c.target)) << c.text;
    if (c.value != nullptr) EXPECT_EQ(f.target.value, c.value) << c.text;
}

INSTANTIATE_TEST_SUITE_P(
    Spec, RuleParserCases,
    ::testing::Values(
        Case{"Why is my Mac running slowly?", Intent::diagnose_slow, TargetKind::host, nullptr},
        Case{"What process is consuming the most CPU?", Intent::top_cpu, TargetKind::host, nullptr},
        Case{"Explain why this application uses so much memory.", Intent::process_memory, TargetKind::reference, nullptr},
        Case{"How does the scheduler affect this workload?", Intent::explain_concept, TargetKind::none, nullptr},
        Case{"Which service owns these processes?", Intent::process_tree, TargetKind::reference, nullptr},
        Case{"What happens if I change this CPU limit?", Intent::what_if, TargetKind::host, nullptr},
        Case{"Why is this container being throttled?", Intent::container_throttle, TargetKind::host, nullptr},
        Case{"Can this Kubernetes workload be optimized without compromising availability?", Intent::kubernetes,
             TargetKind::none, nullptr},
        Case{"Why is my CPU running hot when only two applications are open?", Intent::diagnose_cpu, TargetKind::host,
             nullptr},
        Case{"What is using CPU?", Intent::top_cpu, TargetKind::host, nullptr},
        Case{"Which one is using the most memory?", Intent::top_memory, TargetKind::host, nullptr},
        Case{"Why might it be consuming so much memory?", Intent::process_memory, TargetKind::reference, "it"},
        Case{"why is google chrome using so much cpu", Intent::process_cpu, TargetKind::process_name, "google chrome"},
        Case{"tell me about pid 4242", Intent::process_info, TargetKind::pid, "4242"},
        Case{"kill firefox", Intent::action_request, TargetKind::process_name, "firefox"},
        Case{"how many cores do I have", Intent::cpu_topology, TargetKind::host, nullptr},
        Case{"how much disk space is left", Intent::disk_usage, TargetKind::host, nullptr},
        Case{"write me a poem about cats", Intent::unsupported, TargetKind::none, nullptr}));

TEST(Frame, ObservationsDerivedFromIntentNotParser) {
    auto reg = CapabilityRegistry::builtin();
    test::MockOs mac(Platform::macos);
    test::MockOs lin(Platform::linux_os);
    ParseOutput p{Intent::diagnose_cpu, 0.9F, {}, "test"};
    auto toks = tokenize("why is my cpu busy");
    auto fm = build_frame(p, toks, Platform::macos, mac.supported(), reg);
    auto fl = build_frame(p, toks, Platform::linux_os, lin.supported(), reg);
    auto contains = [](const std::vector<ObservationKind>& v, ObservationKind k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    EXPECT_TRUE(contains(fm.observations, ObservationKind::thermal_state));
    EXPECT_FALSE(contains(fm.observations, ObservationKind::pressure_stall));
    ASSERT_FALSE(fm.unavailable_observations.empty());
    EXPECT_NE(fm.unavailable_observations[0].find("not available on macos"), std::string::npos);
    EXPECT_TRUE(contains(fl.observations, ObservationKind::pressure_stall));
    EXPECT_FALSE(contains(fl.observations, ObservationKind::thermal_state));
    for (const auto& c : fm.capabilities) EXPECT_EQ(c.rfind("observe.", 0), 0U);
    EXPECT_TRUE(validate(fm).empty());
}

TEST(Frame, ActionMapsToDeniedCapability) {
    auto reg = CapabilityRegistry::builtin();
    test::MockOs os;
    auto toks = tokenize("please renice pid 300");
    RuleParser p;
    auto f = build_frame(p.parse(toks), toks, Platform::macos, os.supported(), reg);
    EXPECT_EQ(f.intent, Intent::action_request);
    EXPECT_EQ(f.action, ActionType::set_priority);
    ASSERT_EQ(f.capabilities.size(), 1U);
    EXPECT_EQ(f.capabilities[0], "act.process.set_priority");
    EXPECT_TRUE(f.read_only);
}

TEST(Frame, ValidationCatchesBrokenFrames) {
    IntentFrame f;
    f.intent = Intent::process_cpu;
    f.target.kind = TargetKind::host;
    f.read_only = false;
    f.top_n = 500;
    f.action = ActionType::terminate;
    auto v = validate(f);
    EXPECT_GE(v.size(), 4U);
    IntentFrame g;
    g.intent = Intent::process_info;
    g.target = Target{TargetKind::pid, "12a", std::nullopt};
    EXPECT_FALSE(validate(g).empty());
}

TEST(Frame, JsonMatchesSchemaShape) {
    auto reg = CapabilityRegistry::builtin();
    test::MockOs os;
    RuleParser p;
    auto toks = tokenize("Why is my CPU running hot when only two applications are open?");
    auto f = build_frame(p.parse(toks), toks, Platform::macos, os.supported(), reg);
    auto j = to_json(f);
    for (const char* k : {"\"intent\":\"diagnose_cpu\"", "\"target\":{\"kind\":\"host\"}", "\"read_only\":true",
                          "\"observations\":[", "\"cpu_times\"", "\"unavailable_observations\":["})
        EXPECT_NE(j.find(k), std::string::npos) << k << " in " << j;
}

TEST(Lexicons, ConceptsAndOrdinals) {
    EXPECT_EQ(find_concept(tokenize("what is the run queue")), "run_queue");
    EXPECT_EQ(find_concept(tokenize("explain memory compression on mac")), "memory_compression");
    EXPECT_FALSE(find_concept(tokenize("hello there")));
    EXPECT_EQ(parse_ordinal("second"), 2);
    EXPECT_EQ(parse_ordinal("last"), -1);
    EXPECT_EQ(find_action(tokenize("kill it")), ActionType::terminate);
    EXPECT_EQ(find_action(tokenize("set swappiness to 10")), ActionType::kernel_parameter);
}
