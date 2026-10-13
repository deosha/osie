// SPDX-License-Identifier: Apache-2.0
// libFuzzer harness for the .osiewm loader (docs/WORLD_MODEL.md). Any accepted
// model must evaluate a fixed history without crashing and return finite,
// bounded, ordered predictions (or abstain).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "osie/prediction/world_model.hpp"

using namespace osie::predict;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto m = WorldModel::load_from_memory(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!m) return 0;
    CgroupWindow w;
    w.secs = 0.5;
    w.usage_cores = 1.2;
    w.throttled_ratio = 0.4;
    w.throttled_frac = 0.6;
    w.psi_some = 0.2;
    w.psi_full = 0.1;
    w.quota_cores = 1.5;
    w.cpuset_cpus = 4;
    w.pids = 6;
    w.host_busy_frac = 0.3;
    w.host_cpus = 8;
    const std::vector<CgroupWindow> h(8, w);
    for (auto a : {Intervention{ActionKind::cpu_quota, 1.5, 0.7}, Intervention{ActionKind::cpuset, 4, 2},
                   Intervention{ActionKind::none, 0, 0}, Intervention{ActionKind::offered_load, 1, 2}}) {
        auto p = (*m)->predict_in(h, a, RuntimeEnvironment{"x86_64", "6.8.0", 2});
        if (!p) continue;
        for (const auto& t : p->targets) {
            if (!std::isfinite(t.expected) || !std::isfinite(t.current)) std::abort();
            for (const auto& iv : t.intervals)
                if (!std::isfinite(iv.lo) || !std::isfinite(iv.hi) || iv.lo > iv.hi + 1e-9) std::abort();
        }
    }
    return 0;
}
