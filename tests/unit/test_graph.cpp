// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "osie/architecture/model.hpp"
#include "support/mock_os.hpp"

using namespace osie;
using namespace osie::arch;
using osie::test::proc;

TEST(Graph, SchemaIsEnforced) {
    KnowledgeGraph g;
    g.begin_update();
    auto p = g.upsert(NodeKind::process, "pid:1", "init");
    auto c = g.upsert(NodeKind::logical_cpu, "cpu:0", "cpu0");
    EXPECT_FALSE(g.link(p, EdgeKind::parent_of, c, EdgeNature::observed)) << "process PARENT_OF cpu is invalid";
    EXPECT_FALSE(g.link(c, EdgeKind::has_thread, p, EdgeNature::observed));
    auto t = g.upsert(NodeKind::thread, "tid:7", "t");
    EXPECT_TRUE(g.link(p, EdgeKind::has_thread, t, EdgeNature::observed));
    EXPECT_TRUE(g.link(t, EdgeKind::runs_on, c, EdgeNature::observed));
    EXPECT_TRUE(g.check_consistency().empty());
}

TEST(Graph, UpsertIsIdempotentAndLinkDeduplicates) {
    KnowledgeGraph g;
    g.begin_update();
    auto a = g.upsert(NodeKind::process, "pid:1", "a");
    auto a2 = g.upsert(NodeKind::process, "pid:1", "a");
    EXPECT_EQ(a, a2);
    auto b = g.upsert(NodeKind::process, "pid:2", "b");
    auto e1 = g.link(a, EdgeKind::parent_of, b, EdgeNature::observed);
    auto e2 = g.link(a, EdgeKind::parent_of, b, EdgeNature::observed);
    EXPECT_EQ(*e1, *e2);
    EXPECT_EQ(g.stats().edges_alive, 1U);
}

TEST(Graph, TemporalValidityAndRetirement) {
    KnowledgeGraph g;
    g.begin_update();
    auto a = g.upsert(NodeKind::process, "pid:1", "a");
    auto b = g.upsert(NodeKind::process, "pid:2", "b");
    g.link(a, EdgeKind::parent_of, b, EdgeNature::observed);
    g.begin_update();
    g.upsert(NodeKind::process, "pid:1", "a");  // pid 2 exited
    EXPECT_EQ(g.retire_untouched(NodeKind::process), 1U);
    EXPECT_EQ(g.find(NodeKind::process, "pid:2"), nullptr);
    EXPECT_TRUE(g.out(a, EdgeKind::parent_of).empty()) << "edges to retired nodes are retired too";
    EXPECT_TRUE(g.check_consistency().empty());
    const auto total = g.stats().nodes_total;
    g.compact();
    EXPECT_EQ(g.stats().nodes_total, total - 1);
    EXPECT_NE(g.find(NodeKind::process, "pid:1"), nullptr);
}

TEST(Graph, DetectsParentCycles) {
    KnowledgeGraph g;
    g.begin_update();
    auto a = g.upsert(NodeKind::process, "pid:1", "a");
    auto b = g.upsert(NodeKind::process, "pid:2", "b");
    g.link(a, EdgeKind::parent_of, b, EdgeNature::observed);
    g.link(b, EdgeKind::parent_of, a, EdgeNature::observed);
    auto v = g.check_consistency();
    ASSERT_FALSE(v.empty());
    EXPECT_NE(v.front().find("cycle"), std::string::npos);
}

TEST(Ontology, LoadsConsistentlyAndIsIdempotent) {
    KnowledgeGraph g;
    load_ontology(g);
    const auto s1 = g.stats();
    load_ontology(g);
    EXPECT_EQ(g.stats().edges_alive, s1.edges_alive);
    EXPECT_GE(s1.by_kind[static_cast<std::size_t>(NodeKind::concept_)], 30U);
    auto v = g.check_consistency();
    EXPECT_TRUE(v.empty()) << (v.empty() ? "" : v.front());
}

TEST(Ontology, PlatformAwarePaths) {
    KnowledgeGraph g;
    load_ontology(g);
    const auto quota = concept_node(g, concept_key::cpu_quota);
    const auto lat = concept_node(g, concept_key::scheduling_latency);
    EXPECT_TRUE(g.path(quota, lat, Platform::linux_os));
    EXPECT_FALSE(g.path(quota, lat, Platform::macos)) << "cgroup bandwidth control is Linux-only";
    const auto mp = concept_node(g, concept_key::memory_pressure);
    const auto comp = concept_node(g, concept_key::memory_compression);
    EXPECT_TRUE(g.path(mp, comp, Platform::macos));
    EXPECT_FALSE(g.path(mp, comp, Platform::linux_os));
    // Invariants vs hypotheses are distinguished.
    bool saw_inv = false, saw_hyp = false;
    for (auto e : g.consequences(mp, Platform::macos, 3)) {
        saw_inv |= g.edge(e).nature == EdgeNature::invariant;
        saw_hyp |= g.edge(e).nature == EdgeNature::hypothesized;
        if (g.edge(e).nature == EdgeNature::hypothesized) EXPECT_LT(g.edge(e).confidence, 1.0F);
    }
    EXPECT_TRUE(saw_inv);
    EXPECT_TRUE(saw_hyp);
}

TEST(Instances, BuildAndIncrementalUpdate) {
    KnowledgeGraph g;
    load_ontology(g);
    SystemState s;
    SystemInfo si;
    si.hostname = "h";
    s.system = Observation<SystemInfo>{si, Provenance::now("t")};
    auto p1 = proc(1, 0, "launchd", 0, 10, 0, 0);
    auto p2 = proc(50, 1, "app", 0, 100);
    auto p3 = proc(51, 50, "helper", 0, 100);
    p3.cgroup = "/system.slice/nginx.service";
    s.processes = Observation<std::vector<ProcessView>>{{{p1, {}, false}, {p2, {}, false}, {p3, {}, false}}, Provenance::now("t")};
    auto rep = update_instances(g, s, Platform::linux_os);
    EXPECT_EQ(rep.processes, 3U);
    EXPECT_EQ(rep.parent_edges, 2U);
    EXPECT_EQ(rep.services, 1U);
    const auto* svc = g.find(NodeKind::service, "nginx.service");
    ASSERT_NE(svc, nullptr);
    ASSERT_EQ(g.out(svc->id, EdgeKind::manages).size(), 2U);  // process + cgroup
    for (auto e : g.out_edges(svc->id)) EXPECT_EQ(g.edge(e).nature, EdgeNature::inferred);
    EXPECT_TRUE(g.check_consistency().empty());

    // Helper exits; app remains.
    s.processes = Observation<std::vector<ProcessView>>{{{p1, {}, false}, {p2, {}, false}}, Provenance::now("t")};
    rep = update_instances(g, s, Platform::linux_os);
    EXPECT_GE(rep.retired, 1U);
    EXPECT_EQ(g.nodes_of(NodeKind::process).size(), 2U);
    EXPECT_EQ(g.find(NodeKind::service, "nginx.service"), nullptr);
    EXPECT_TRUE(g.check_consistency().empty());
}

TEST(Instances, PidReuseCreatesDistinctIdentity) {
    KnowledgeGraph g;
    SystemState s;
    auto a = proc(77, 1, "old", 0, 1, 1000);
    s.processes = Observation<std::vector<ProcessView>>{{{a, {}, false}}, Provenance::now("t")};
    update_instances(g, s, Platform::macos);
    auto b = proc(77, 1, "new", 0, 1, 5000);
    s.processes = Observation<std::vector<ProcessView>>{{{b, {}, false}}, Provenance::now("t")};
    update_instances(g, s, Platform::macos);
    const auto alive = g.nodes_of(NodeKind::process);
    ASSERT_EQ(alive.size(), 1U);
    EXPECT_EQ(g.node(alive[0]).label, "new");
    EXPECT_EQ(g.stats().nodes_total - g.stats().nodes_alive, 1U);  // the old (pid 77, start 1000) identity
}

TEST(Systemd, UnitFromCgroupPath) {
    EXPECT_EQ(systemd_unit_from_cgroup("/system.slice/nginx.service"), "nginx.service");
    EXPECT_EQ(systemd_unit_from_cgroup("/user.slice/user-1000.slice/session-2.scope"), "session-2.scope");
    EXPECT_EQ(systemd_unit_from_cgroup("/system.slice/docker-abc.scope/init"), "docker-abc.scope");
    EXPECT_FALSE(systemd_unit_from_cgroup("/"));
    EXPECT_FALSE(systemd_unit_from_cgroup("/kubepods/burstable/pod1"));
}

TEST(Ontology, ExperimentalEdgesAreLabelledAndReferenced) {
    KnowledgeGraph g;
    load_ontology(g);
    std::size_t experimental = 0;
    for (const auto* n = g.find(NodeKind::concept_, concept_key::cpu_quota); n != nullptr; n = nullptr)
        for (auto eid : g.out_edges(n->id)) {
            const auto& e = g.edge(eid);
            if (e.nature != EdgeNature::experimental) continue;
            ++experimental;
            EXPECT_NE(e.doc_ref.find("WORLD_MODEL_RESULTS"), std::string::npos);
            EXPECT_LT(e.confidence, 1.0F);
        }
    EXPECT_GE(experimental, 1U);
    EXPECT_TRUE(g.check_consistency().empty());
}
