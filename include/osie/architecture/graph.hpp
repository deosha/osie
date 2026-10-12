// SPDX-License-Identifier: Apache-2.0
// OS architecture knowledge graph.
//
// Two layers share one typed graph:
//  * the static *ontology*: concept nodes (scheduler, run queue, memory
//    compressor, PSI, cgroup CPU bandwidth, ...) and documented relations
//    between them, each with a documentation reference and an EdgeNature that
//    separates hard OS invariants from probabilistic causal links;
//  * the *instance* layer: host, CPUs, processes, users, filesystems,
//    interfaces, cgroups, services, built from live observations.
//
// Storage is data-oriented: nodes and edges live in contiguous vectors indexed
// by 32-bit ids, with per-node adjacency lists and a hash index on (kind,key)
// for O(1) upserts during incremental updates.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "osie/core/observation.hpp"

namespace osie::arch {

enum class NodeKind : std::uint8_t {
    host, cpu_cluster, logical_cpu, process, thread, user, service, memory, swap,
    filesystem, net_interface, cgroup, container, pod, k8s_node, concept_, count_
};
std::string_view to_string(NodeKind k) noexcept;

enum class EdgeKind : std::uint8_t {
    has_thread, parent_of, belongs_to, runs_on, manages, uses, contains, scheduled_on,
    owned_by, part_of, mounted_on, limits, affects, measured_by, count_
};
std::string_view to_string(EdgeKind k) noexcept;

/// Epistemic status of an edge. The reasoning engine must never present an
/// `hypothesized` edge as fact.
enum class EdgeNature : std::uint8_t {
    invariant,     // documented OS architecture fact (always true where platform applies)
    observed,      // read directly from a native interface
    inferred,      // deterministically derived from observations (e.g. unit from cgroup path)
    hypothesized,  // probabilistic causal relation; requires evidence before use
};
std::string_view to_string(EdgeNature n) noexcept;

using NodeId = std::uint32_t;
using EdgeId = std::uint32_t;
inline constexpr NodeId kNoNode = UINT32_MAX;

using AttrValue = std::variant<std::int64_t, double, std::string, bool>;

struct Node {
    NodeId id{kNoNode};
    NodeKind kind{NodeKind::concept_};
    std::string key;    // unique within kind: "pid:123@1700000000", "cpu:3", "memory_pressure"
    std::string label;  // human-readable
    PlatformSet platforms{PlatformSet::all()};
    std::vector<std::pair<std::string, AttrValue>> attrs;  // small; linear scan is cheaper than a map
    std::string_view source;  // provenance (native interface or "ontology")
    std::string doc_ref;      // documentation reference for concept nodes
    std::uint64_t valid_from{0}, valid_to{UINT64_MAX};  // graph revision interval
    std::uint64_t touched{0};                           // last revision that refreshed it
    std::vector<EdgeId> out, in;

    [[nodiscard]] bool alive(std::uint64_t rev) const noexcept { return valid_from <= rev && rev < valid_to; }
    [[nodiscard]] const AttrValue* attr(std::string_view k) const;
    void set_attr(std::string_view k, AttrValue v);
};

struct Edge {
    EdgeId id{0};
    NodeId from{kNoNode}, to{kNoNode};
    EdgeKind kind{EdgeKind::affects};
    EdgeNature nature{EdgeNature::observed};
    PlatformSet platforms{PlatformSet::all()};
    float confidence{1.0F};  // 1.0 for invariant/observed
    std::string_view source;
    std::string doc_ref;
    std::string mechanism;  // one-line explanation used in diagnostics
    std::uint64_t valid_from{0}, valid_to{UINT64_MAX};
    [[nodiscard]] bool alive(std::uint64_t rev) const noexcept { return valid_from <= rev && rev < valid_to; }
};

/// Allowed (from, edge, to) triples: the typed schema the graph enforces.
bool schema_allows(NodeKind from, EdgeKind e, NodeKind to) noexcept;

class KnowledgeGraph {
public:
    /// Starts a new revision. Nodes/edges not touched since begin_update()
    /// can be retired with retire_untouched(kind).
    std::uint64_t begin_update();
    [[nodiscard]] std::uint64_t revision() const noexcept { return rev_; }

    /// Inserts or refreshes a node identified by (kind, key).
    NodeId upsert(NodeKind kind, std::string_view key, std::string_view label, std::string_view source = {});
    /// Adds an edge if schema-valid and not already present (alive) with same (from,kind,to).
    std::optional<EdgeId> link(NodeId from, EdgeKind kind, NodeId to, EdgeNature nature,
                               std::string_view source = {}, float confidence = 1.0F);
    /// Marks nodes of `kind` not refreshed in the current revision as no longer valid,
    /// together with their edges. Returns the number retired.
    std::size_t retire_untouched(NodeKind kind);

    [[nodiscard]] const Node* find(NodeKind kind, std::string_view key) const;
    [[nodiscard]] Node* find_mut(NodeKind kind, std::string_view key);
    [[nodiscard]] const Node& node(NodeId id) const { return nodes_.at(id); }
    Node& node_mut(NodeId id) { return nodes_.at(id); }
    [[nodiscard]] const Edge& edge(EdgeId id) const { return edges_.at(id); }
    Edge& edge_mut(EdgeId id) { return edges_.at(id); }

    [[nodiscard]] std::vector<NodeId> nodes_of(NodeKind kind, bool alive_only = true) const;
    /// Alive outgoing neighbours over edges of `kind`.
    [[nodiscard]] std::vector<NodeId> out(NodeId n, EdgeKind kind) const;
    [[nodiscard]] std::vector<NodeId> in(NodeId n, EdgeKind kind) const;
    [[nodiscard]] std::vector<EdgeId> out_edges(NodeId n) const;

    /// Breadth-first path over alive edges whose kind is in `kinds` (empty = any)
    /// and whose platforms include `platform`. Returns edge ids from `from` to `to`.
    [[nodiscard]] std::optional<std::vector<EdgeId>> path(NodeId from, NodeId to, Platform platform,
                                                          const std::vector<EdgeKind>& kinds = {},
                                                          std::size_t max_depth = 6) const;
    /// All alive concept nodes reachable from `from` through `affects` edges.
    [[nodiscard]] std::vector<EdgeId> consequences(NodeId from, Platform platform, std::size_t max_depth = 3) const;

    /// Structural consistency checks: schema, dangling ids, parent_of cycles,
    /// edges alive while an endpoint is dead. Returns violations.
    [[nodiscard]] std::vector<std::string> check_consistency() const;

    struct Stats {
        std::size_t nodes_alive{0}, edges_alive{0}, nodes_total{0}, edges_total{0};
        std::size_t by_kind[static_cast<std::size_t>(NodeKind::count_)]{};
    };
    [[nodiscard]] Stats stats() const;

    /// Graphviz export (alive elements only), for documentation/debugging.
    [[nodiscard]] std::string to_dot(bool concepts_only = false) const;

    /// Drops retired nodes/edges older than the current revision (compaction).
    /// Ids are re-assigned; callers must not keep ids across compaction.
    void compact();

private:
    static std::string index_key(NodeKind k, std::string_view key);
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, NodeId> index_;  // "kind|key" -> id
    std::uint64_t rev_{0};
};

}  // namespace osie::arch
