// SPDX-License-Identifier: Apache-2.0
#include "osie/architecture/graph.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <sstream>
#include <utility>

namespace osie::arch {

std::string_view to_string(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::host: return "host";
        case NodeKind::cpu_cluster: return "cpu_cluster";
        case NodeKind::logical_cpu: return "logical_cpu";
        case NodeKind::process: return "process";
        case NodeKind::thread: return "thread";
        case NodeKind::user: return "user";
        case NodeKind::service: return "service";
        case NodeKind::memory: return "memory";
        case NodeKind::swap: return "swap";
        case NodeKind::filesystem: return "filesystem";
        case NodeKind::net_interface: return "net_interface";
        case NodeKind::cgroup: return "cgroup";
        case NodeKind::container: return "container";
        case NodeKind::pod: return "pod";
        case NodeKind::k8s_node: return "k8s_node";
        case NodeKind::concept_: return "concept";
        case NodeKind::count_: break;
    }
    return "?";
}

std::string_view to_string(EdgeKind k) noexcept {
    switch (k) {
        case EdgeKind::has_thread: return "HAS_THREAD";
        case EdgeKind::parent_of: return "PARENT_OF";
        case EdgeKind::belongs_to: return "BELONGS_TO";
        case EdgeKind::runs_on: return "RUNS_ON";
        case EdgeKind::manages: return "MANAGES";
        case EdgeKind::uses: return "USES";
        case EdgeKind::contains: return "CONTAINS";
        case EdgeKind::scheduled_on: return "SCHEDULED_ON";
        case EdgeKind::owned_by: return "OWNED_BY";
        case EdgeKind::part_of: return "PART_OF";
        case EdgeKind::mounted_on: return "MOUNTED_ON";
        case EdgeKind::limits: return "LIMITS";
        case EdgeKind::affects: return "AFFECTS";
        case EdgeKind::measured_by: return "MEASURED_BY";
        case EdgeKind::count_: break;
    }
    return "?";
}

std::string_view to_string(EdgeNature n) noexcept {
    switch (n) {
        case EdgeNature::invariant: return "invariant";
        case EdgeNature::observed: return "observed";
        case EdgeNature::inferred: return "inferred";
        case EdgeNature::hypothesized: return "hypothesized";
    }
    return "?";
}

bool schema_allows(NodeKind f, EdgeKind e, NodeKind t) noexcept {
    using N = NodeKind;
    using E = EdgeKind;
    struct Triple { N f; E e; N t; };
    static constexpr Triple allowed[] = {
        {N::host, E::contains, N::cpu_cluster},   {N::host, E::contains, N::memory},
        {N::host, E::contains, N::swap},          {N::host, E::contains, N::filesystem},
        {N::host, E::contains, N::net_interface}, {N::host, E::contains, N::cgroup},
        {N::host, E::contains, N::container},     {N::cpu_cluster, E::contains, N::logical_cpu},
        {N::process, E::parent_of, N::process},   {N::process, E::has_thread, N::thread},
        {N::process, E::part_of, N::host},        {N::thread, E::runs_on, N::logical_cpu},
        {N::process, E::owned_by, N::user},       {N::process, E::belongs_to, N::cgroup},
        {N::cgroup, E::part_of, N::cgroup},       {N::cgroup, E::limits, N::process},
        {N::service, E::manages, N::process},     {N::service, E::manages, N::cgroup},
        {N::process, E::uses, N::memory},         {N::process, E::uses, N::swap},
        {N::process, E::uses, N::filesystem},     {N::process, E::uses, N::net_interface},
        {N::memory, E::uses, N::swap},            {N::container, E::contains, N::process},
        {N::container, E::belongs_to, N::cgroup}, {N::pod, E::contains, N::container},
        {N::pod, E::scheduled_on, N::k8s_node},   {N::host, E::part_of, N::k8s_node},
        {N::concept_, E::affects, N::concept_},   {N::concept_, E::limits, N::concept_},
        {N::concept_, E::part_of, N::concept_},   {N::concept_, E::measured_by, N::concept_},
    };
    for (const auto& a : allowed)
        if (a.f == f && a.e == e && a.t == t) return true;
    // Any instance may be classified under a concept (e.g. logical_cpu PART_OF concept:cpu).
    return e == E::part_of && t == N::concept_;
}

const AttrValue* Node::attr(std::string_view k) const {
    for (const auto& [name, v] : attrs)
        if (name == k) return &v;
    return nullptr;
}

void Node::set_attr(std::string_view k, AttrValue v) {
    for (auto& [name, val] : attrs) {
        if (name == k) {
            val = std::move(v);
            return;
        }
    }
    attrs.emplace_back(std::string(k), std::move(v));
}

std::string KnowledgeGraph::index_key(NodeKind k, std::string_view key) {
    std::string s;
    s.reserve(key.size() + 3);
    s += static_cast<char>('A' + static_cast<int>(k));
    s += '|';
    s += key;
    return s;
}

std::uint64_t KnowledgeGraph::begin_update() { return ++rev_; }

NodeId KnowledgeGraph::upsert(NodeKind kind, std::string_view key, std::string_view label, std::string_view source) {
    auto ik = index_key(kind, key);
    if (auto it = index_.find(ik); it != index_.end()) {
        Node& n = nodes_[it->second];
        if (n.valid_to != UINT64_MAX) {
            // Previously retired with the same identity: start a fresh validity interval.
            n.valid_from = rev_;
            n.valid_to = UINT64_MAX;
        }
        n.touched = rev_;
        if (!label.empty()) n.label = label;
        if (!source.empty()) n.source = source;
        return n.id;
    }
    Node n;
    n.id = static_cast<NodeId>(nodes_.size());
    n.kind = kind;
    n.key = key;
    n.label = label.empty() ? std::string(key) : std::string(label);
    n.source = source;
    n.valid_from = rev_;
    n.touched = rev_;
    index_.emplace(std::move(ik), n.id);
    nodes_.push_back(std::move(n));
    return nodes_.back().id;
}

std::optional<EdgeId> KnowledgeGraph::link(NodeId from, EdgeKind kind, NodeId to, EdgeNature nature,
                                           std::string_view source, float confidence) {
    if (from >= nodes_.size() || to >= nodes_.size()) return std::nullopt;
    if (!schema_allows(nodes_[from].kind, kind, nodes_[to].kind)) return std::nullopt;
    for (EdgeId e : nodes_[from].out) {
        Edge& ed = edges_[e];
        if (ed.to == to && ed.kind == kind && ed.alive(rev_)) {
            ed.nature = nature;
            ed.confidence = confidence;
            return e;
        }
    }
    Edge e;
    e.id = static_cast<EdgeId>(edges_.size());
    e.from = from;
    e.to = to;
    e.kind = kind;
    e.nature = nature;
    e.source = source;
    e.confidence = confidence;
    e.valid_from = rev_;
    nodes_[from].out.push_back(e.id);
    nodes_[to].in.push_back(e.id);
    edges_.push_back(std::move(e));
    return edges_.back().id;
}

std::size_t KnowledgeGraph::retire_untouched(NodeKind kind) {
    std::size_t n = 0;
    for (auto& node : nodes_) {
        if (node.kind != kind || node.valid_to != UINT64_MAX || node.touched >= rev_) continue;
        node.valid_to = rev_;
        ++n;
        for (EdgeId e : node.out)
            if (edges_[e].valid_to == UINT64_MAX) edges_[e].valid_to = rev_;
        for (EdgeId e : node.in)
            if (edges_[e].valid_to == UINT64_MAX) edges_[e].valid_to = rev_;
    }
    return n;
}

const Node* KnowledgeGraph::find(NodeKind kind, std::string_view key) const {
    auto it = index_.find(index_key(kind, key));
    if (it == index_.end()) return nullptr;
    const Node& n = nodes_[it->second];
    return n.alive(rev_) ? &n : nullptr;
}

Node* KnowledgeGraph::find_mut(NodeKind kind, std::string_view key) {
    return const_cast<Node*>(std::as_const(*this).find(kind, key));
}

std::vector<NodeId> KnowledgeGraph::nodes_of(NodeKind kind, bool alive_only) const {
    std::vector<NodeId> v;
    for (const auto& n : nodes_)
        if (n.kind == kind && (!alive_only || n.alive(rev_))) v.push_back(n.id);
    return v;
}

std::vector<NodeId> KnowledgeGraph::out(NodeId n, EdgeKind kind) const {
    std::vector<NodeId> v;
    for (EdgeId e : nodes_.at(n).out) {
        const Edge& ed = edges_[e];
        if (ed.kind == kind && ed.alive(rev_)) v.push_back(ed.to);
    }
    return v;
}

std::vector<NodeId> KnowledgeGraph::in(NodeId n, EdgeKind kind) const {
    std::vector<NodeId> v;
    for (EdgeId e : nodes_.at(n).in) {
        const Edge& ed = edges_[e];
        if (ed.kind == kind && ed.alive(rev_)) v.push_back(ed.from);
    }
    return v;
}

std::vector<EdgeId> KnowledgeGraph::out_edges(NodeId n) const {
    std::vector<EdgeId> v;
    for (EdgeId e : nodes_.at(n).out)
        if (edges_[e].alive(rev_)) v.push_back(e);
    return v;
}

std::optional<std::vector<EdgeId>> KnowledgeGraph::path(NodeId from, NodeId to, Platform platform,
                                                        const std::vector<EdgeKind>& kinds,
                                                        std::size_t max_depth) const {
    if (from >= nodes_.size() || to >= nodes_.size()) return std::nullopt;
    if (from == to) return std::vector<EdgeId>{};
    std::vector<EdgeId> via(nodes_.size(), UINT32_MAX);
    std::vector<std::uint8_t> depth(nodes_.size(), 0);
    std::vector<bool> seen(nodes_.size(), false);
    std::deque<NodeId> q{from};
    seen[from] = true;
    while (!q.empty()) {
        const NodeId cur = q.front();
        q.pop_front();
        if (depth[cur] >= max_depth) continue;
        for (EdgeId e : nodes_[cur].out) {
            const Edge& ed = edges_[e];
            if (!ed.alive(rev_) || !ed.platforms.contains(platform) || seen[ed.to]) continue;
            if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), ed.kind) == kinds.end()) continue;
            seen[ed.to] = true;
            via[ed.to] = e;
            depth[ed.to] = static_cast<std::uint8_t>(depth[cur] + 1);
            if (ed.to == to) {
                std::vector<EdgeId> p;
                for (NodeId n = to; n != from; n = edges_[via[n]].from) p.push_back(via[n]);
                std::reverse(p.begin(), p.end());
                return p;
            }
            q.push_back(ed.to);
        }
    }
    return std::nullopt;
}

std::vector<EdgeId> KnowledgeGraph::consequences(NodeId from, Platform platform, std::size_t max_depth) const {
    std::vector<EdgeId> out_e;
    std::vector<bool> seen(nodes_.size(), false);
    std::deque<std::pair<NodeId, std::size_t>> q{{from, 0}};
    seen.at(from) = true;
    while (!q.empty()) {
        auto [cur, d] = q.front();
        q.pop_front();
        if (d >= max_depth) continue;
        for (EdgeId e : nodes_[cur].out) {
            const Edge& ed = edges_[e];
            if (ed.kind != EdgeKind::affects || !ed.alive(rev_) || !ed.platforms.contains(platform)) continue;
            out_e.push_back(e);
            if (!seen[ed.to]) {
                seen[ed.to] = true;
                q.emplace_back(ed.to, d + 1);
            }
        }
    }
    return out_e;
}

std::vector<std::string> KnowledgeGraph::check_consistency() const {
    std::vector<std::string> v;
    for (const auto& e : edges_) {
        if (e.from >= nodes_.size() || e.to >= nodes_.size()) {
            v.push_back("edge " + std::to_string(e.id) + " has dangling endpoint");
            continue;
        }
        const Node& a = nodes_[e.from];
        const Node& b = nodes_[e.to];
        if (!schema_allows(a.kind, e.kind, b.kind)) {
            v.push_back("schema violation: " + std::string(to_string(a.kind)) + " " + std::string(to_string(e.kind)) +
                        " " + std::string(to_string(b.kind)));
        }
        if (e.alive(rev_) && (!a.alive(rev_) || !b.alive(rev_))) {
            v.push_back("alive edge " + std::string(to_string(e.kind)) + " touches retired node " + a.key + "->" + b.key);
        }
        if (e.nature == EdgeNature::hypothesized && e.confidence >= 1.0F) {
            v.push_back("hypothesized edge with certainty 1.0: " + a.key + "->" + b.key);
        }
        if ((e.nature == EdgeNature::invariant || e.nature == EdgeNature::hypothesized) &&
            a.kind == NodeKind::concept_ && e.doc_ref.empty()) {
            v.push_back("ontology edge without documentation reference: " + a.key + "->" + b.key);
        }
    }
    // PARENT_OF must be acyclic (process tree). Iterative colouring DFS.
    std::vector<std::uint8_t> colour(nodes_.size(), 0);
    for (const auto& n : nodes_) {
        if (n.kind != NodeKind::process || !n.alive(rev_) || colour[n.id] != 0) continue;
        std::vector<std::pair<NodeId, std::size_t>> stack{{n.id, 0}};
        colour[n.id] = 1;
        while (!stack.empty()) {
            auto& [cur, idx] = stack.back();
            const auto& outs = nodes_[cur].out;
            if (idx >= outs.size()) {
                colour[cur] = 2;
                stack.pop_back();
                continue;
            }
            const Edge& ed = edges_[outs[idx++]];
            if (ed.kind != EdgeKind::parent_of || !ed.alive(rev_)) continue;
            if (colour[ed.to] == 1) {
                v.push_back("PARENT_OF cycle through " + nodes_[ed.to].key);
            } else if (colour[ed.to] == 0) {
                colour[ed.to] = 1;
                stack.emplace_back(ed.to, 0);
            }
        }
    }
    return v;
}

KnowledgeGraph::Stats KnowledgeGraph::stats() const {
    Stats s;
    s.nodes_total = nodes_.size();
    s.edges_total = edges_.size();
    for (const auto& n : nodes_) {
        if (!n.alive(rev_)) continue;
        ++s.nodes_alive;
        ++s.by_kind[static_cast<std::size_t>(n.kind)];
    }
    for (const auto& e : edges_)
        if (e.alive(rev_)) ++s.edges_alive;
    return s;
}

std::string KnowledgeGraph::to_dot(bool concepts_only) const {
    std::ostringstream o;
    o << "digraph osie {\n  rankdir=LR; node [shape=box,fontsize=10];\n";
    auto include = [&](const Node& n) { return n.alive(rev_) && (!concepts_only || n.kind == NodeKind::concept_); };
    for (const auto& n : nodes_) {
        if (!include(n)) continue;
        o << "  n" << n.id << " [label=\"" << to_string(n.kind) << "\\n" << n.label << "\"];\n";
    }
    for (const auto& e : edges_) {
        if (!e.alive(rev_) || !include(nodes_[e.from]) || !include(nodes_[e.to])) continue;
        o << "  n" << e.from << " -> n" << e.to << " [label=\"" << to_string(e.kind) << "\"";
        if (e.nature == EdgeNature::hypothesized) o << ",style=dashed";
        o << "];\n";
    }
    o << "}\n";
    return o.str();
}

void KnowledgeGraph::compact() {
    std::vector<NodeId> remap(nodes_.size(), kNoNode);
    std::vector<Node> nn;
    nn.reserve(nodes_.size());
    for (auto& n : nodes_) {
        if (!n.alive(rev_)) continue;
        remap[n.id] = static_cast<NodeId>(nn.size());
        n.id = remap[n.id];
        n.out.clear();
        n.in.clear();
        nn.push_back(std::move(n));
    }
    std::vector<Edge> ne;
    for (auto& e : edges_) {
        if (!e.alive(rev_) || remap[e.from] == kNoNode || remap[e.to] == kNoNode) continue;
        e.id = static_cast<EdgeId>(ne.size());
        e.from = remap[e.from];
        e.to = remap[e.to];
        nn[e.from].out.push_back(e.id);
        nn[e.to].in.push_back(e.id);
        ne.push_back(std::move(e));
    }
    nodes_ = std::move(nn);
    edges_ = std::move(ne);
    index_.clear();
    for (const auto& n : nodes_) index_.emplace(index_key(n.kind, n.key), n.id);
}

}  // namespace osie::arch
