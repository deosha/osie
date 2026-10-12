// SPDX-License-Identifier: Apache-2.0
// Static OS ontology + instance-graph construction from live state.
#pragma once

#include <string_view>

#include "osie/architecture/graph.hpp"
#include "osie/state/state.hpp"

namespace osie::arch {

/// Stable concept keys used by the reasoning engine.
namespace concept_key {
inline constexpr std::string_view cpu = "cpu";
inline constexpr std::string_view scheduler = "scheduler";
inline constexpr std::string_view run_queue = "run_queue";
inline constexpr std::string_view cpu_saturation = "cpu_saturation";
inline constexpr std::string_view scheduling_latency = "scheduling_latency";
inline constexpr std::string_view load_average = "load_average";
inline constexpr std::string_view heterogeneous_cores = "heterogeneous_cores";
inline constexpr std::string_view qos_classes = "qos_classes";
inline constexpr std::string_view thermal_pressure = "thermal_pressure";
inline constexpr std::string_view cpu_frequency = "cpu_frequency";
inline constexpr std::string_view cpu_quota = "cgroup_cpu_quota";
inline constexpr std::string_view cpu_throttling = "cpu_bandwidth_throttling";
inline constexpr std::string_view psi_cpu = "psi_cpu";
inline constexpr std::string_view hypervisor_steal = "hypervisor_steal";
inline constexpr std::string_view memory = "memory";
inline constexpr std::string_view physical_memory = "physical_memory";
inline constexpr std::string_view virtual_memory = "virtual_memory";
inline constexpr std::string_view page_cache = "page_cache";
inline constexpr std::string_view memory_pressure = "memory_pressure";
inline constexpr std::string_view memory_compression = "memory_compression";
inline constexpr std::string_view paging = "paging";
inline constexpr std::string_view swapping = "swapping";
inline constexpr std::string_view major_faults = "major_page_faults";
inline constexpr std::string_view oom = "out_of_memory_kill";
inline constexpr std::string_view vm_pressure_level = "vm_pressure_level";
inline constexpr std::string_view psi_memory = "psi_memory";
inline constexpr std::string_view memory_limit = "cgroup_memory_limit";
inline constexpr std::string_view disk_io = "disk_io";
inline constexpr std::string_view io_latency = "io_latency";
inline constexpr std::string_view psi_io = "psi_io";
inline constexpr std::string_view responsiveness = "application_responsiveness";
}  // namespace concept_key

/// Loads concept nodes and documented relations. Idempotent.
void load_ontology(KnowledgeGraph& g);

NodeId concept_node(const KnowledgeGraph& g, std::string_view key);

struct BuildReport {
    std::size_t processes{0}, parent_edges{0}, users{0}, cgroups{0}, services{0};
    std::size_t retired{0};
};

/// Incrementally updates the instance layer from a state sample: upserts
/// nodes keyed by stable identities, then retires nodes that disappeared.
BuildReport update_instances(KnowledgeGraph& g, const SystemState& s, Platform platform);

/// Derives a systemd unit name from a cgroup v2 path, e.g.
/// "/system.slice/nginx.service" -> "nginx.service". Documented systemd
/// convention (systemd.unit(5), "Control Group hierarchy").
std::optional<std::string> systemd_unit_from_cgroup(std::string_view path);

}  // namespace osie::arch
