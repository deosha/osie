// SPDX-License-Identifier: Apache-2.0
#include "osie/engine/serialize.hpp"

namespace osie::serialize {

void write_trend(json::Writer& w, const std::string& label, const TrendResult& t) { write_trend(w, label, t, true); }

void write_trend(json::Writer& w, const std::string& label, const TrendResult& t, bool close) {
    w.begin_object().kv("series", label).kv("direction", to_string(t.direction)).kv("samples", static_cast<std::uint64_t>(t.n))
        .kv("span_s", t.span_s);
    if (t.direction == TrendDirection::insufficient_data) w.kv("why", t.why);
    else w.kv("first", t.first).kv("last", t.last).kv("early_mean", t.early_mean).kv("late_mean", t.late_mean)
             .kv("slope_per_min", t.slope_per_min).kv("mann_kendall_z", t.mann_kendall_z);
    if (close) w.end_object();
}

void write_unavailable(json::Writer& w, const Unavailable& u, bool detailed) {
    w.begin_object().kv("kind", to_string(u.kind));
    if (detailed) w.kv("reason", u.why.describe());
    else w.kv("code", to_string(u.why.code));
    w.end_object();
}

std::string answer_json(const Answer& a) {
    json::Writer w;
    w.begin_object();
    w.key("frame");
    intent::write_json(w, a.frame);
    w.key("diagnosis");
    if (a.diagnosis) reason::write_json(w, *a.diagnosis);
    else w.null();
    w.kv("text", a.text).kv("latency_ms", a.latency_ms).kv("parse_ms", a.parse_ms)
        .kv("observation_age_ms", static_cast<std::int64_t>(a.observation_age.count()))
        .kv("reused_observations", a.reused_observations);
    if (!a.trends.empty()) {
        w.key("trends").begin_array();
        for (const auto& [label, t] : a.trends) write_trend(w, label, t);
        w.end_array();
    }
    w.end_object();
    return w.take();
}

std::string snapshot_summary_json(const SystemState& s) {
    json::Writer w;
    w.begin_object();
    if (s.system) w.kv("os", s.system->value.os_name + " " + s.system->value.os_version);
    if (s.cpu) w.kv("cpu_busy_pct", s.cpu->value.busy_pct());
    if (s.load) w.kv("load_1m", s.load->value.one);
    if (s.memory) {
        w.kv("mem_total", s.memory->value.total_bytes).kv("mem_available", s.memory->value.available_bytes);
        if (s.memory->value.pressure_level) w.kv("pressure_level", to_string(*s.memory->value.pressure_level));
    }
    if (s.processes) w.kv("processes", static_cast<std::uint64_t>(s.processes->value.size()))
                         .kv("processes_partial", static_cast<std::uint64_t>(s.processes_partial));
    w.key("unavailable").begin_array();
    for (const auto& u : s.unavailable) write_unavailable(w, u, true);
    w.end_array();
    w.end_object();
    return w.take();
}

}  // namespace osie::serialize
