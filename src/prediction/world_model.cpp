// SPDX-License-Identifier: Apache-2.0
#include "osie/prediction/world_model.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace osie::predict {
namespace {

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> v;
    std::istringstream is{std::string(s)};
    for (std::string w; is >> w;) v.push_back(w);
    return v;
}

bool valid_token(std::string_view s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == '/';
    });
}

std::optional<double> parse_double(const std::string& s) {
    if (s.empty() || s.size() > 40) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

/// Mean of a history quantity with the same definition as the training target.
double current_value(const std::string& target, const std::vector<CgroupWindow>& h) {
    double s = 0;
    for (const auto& w : h) {
        if (target == "usage_cores") s += w.usage_cores;
        else if (target == "throttled_cores") s += w.throttled_frac.value_or(0.0);
        else if (target == "throttled_ratio") s += w.throttled_ratio.value_or(0.0);
        else if (target == "psi_some") s += w.psi_some.value_or(0.0);
    }
    return h.empty() ? 0.0 : s / static_cast<double>(h.size());
}

}  // namespace

Result<std::shared_ptr<WorldModel>> WorldModel::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return make_error(Errc::not_found, "cannot open world model " + path);
    const auto size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > wm_limits::max_file_bytes)
        return make_error(Errc::parse_error, "world model file too large or unreadable");
    std::string text(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    if (!in.read(text.data(), size)) return make_error(Errc::io_error, "cannot read " + path);
    return load_from_memory(text);
}

Result<std::shared_ptr<WorldModel>> WorldModel::load_from_memory(std::string_view text) {
    auto bad = [](std::string m) { return make_error(Errc::parse_error, "world model: " + std::move(m)); };
    if (text.size() > wm_limits::max_file_bytes) return bad("file too large");
    std::shared_ptr<WorldModel> m(new WorldModel());
    std::istringstream is{std::string(text)};
    std::string line;
    if (!std::getline(is, line) || (line != "OSIEWM1" && line != "OSIEWM2"))
        return bad("bad magic (expected OSIEWM1 or OSIEWM2)");
    m->format_ = line == "OSIEWM2" ? 2 : 1;
    std::vector<std::pair<std::string, std::string>> statuses;
    std::vector<std::pair<std::string, double>> gates;
    std::size_t lines = 0;
    std::set<std::string> seen;
    bool ended = false;
    std::vector<std::pair<std::string, std::pair<double, double>>> conformal;  // name, (level, width)
    std::vector<std::pair<std::string, std::array<double, 4>>> regimes;        // name, (level, lo, hi, width)
    std::vector<std::pair<std::string, double>> abstain;
    auto numbers = [&](const std::vector<std::string>& w, std::vector<double>& out) -> bool {
        out.clear();
        out.reserve(w.size() - 1);
        for (std::size_t i = 1; i < w.size(); ++i) {
            auto v = parse_double(w[i]);
            if (!v || std::fabs(*v) > wm_limits::max_abs_weight) return false;
            out.push_back(*v);
        }
        return true;
    };
    while (std::getline(is, line)) {
        if (++lines > wm_limits::max_lines) return bad("too many lines");
        auto w = split_ws(line);
        if (w.empty() || w[0][0] == '#') continue;
        const auto& k = w[0];
        const bool repeatable = k == "target" || k == "conformal" || k == "abstain_width" || k == "action" ||
                                k == "meta" || k == "target_status" || k == "residual_gate" || k == "conformal_regime";
        const bool v2_only = k.rfind("support_", 0) == 0 || k == "feature_schema" || k == "model_version" ||
                             k == "data_version" || k == "calibration_version" || k == "target_status" ||
                             k == "residual_gate" || k == "safe_throttle_max" || k == "workload_coverage" ||
                             k == "conformal_regime";
        if (v2_only && m->format_ < 2) return bad(k + " requires OSIEWM2");
        if (!repeatable && !seen.insert(k).second) return bad("duplicate key " + k);
        if (k == "end") {
            ended = true;
            break;
        } else if (k == "id" && w.size() == 2 && valid_token(w[1])) {
            m->id_ = w[1];
        } else if (k == "kind" && w.size() == 2 && (w[1] == "linear" || w[1] == "mlp")) {
            m->kind_ = w[1];
        } else if (k == "history" && w.size() == 2) {
            auto v = parse_double(w[1]);
            if (!v || *v < 1 || *v > 64 || std::floor(*v) != *v) return bad("history must be 1..64");
            m->history_ = static_cast<std::size_t>(*v);
        } else if (k == "window_s" && w.size() == 2) {
            auto v = parse_double(w[1]);
            if (!v || *v < 0.05 || *v > 60) return bad("window_s must be 0.05..60");
            m->window_s_ = *v;
        } else if (k == "features" && w.size() >= 2 && w.size() - 1 <= wm_limits::max_features) {
            const auto& names = feature_names();
            for (std::size_t i = 1; i < w.size(); ++i) {
                auto it = std::find(names.begin(), names.end(), w[i]);
                if (it == names.end()) return bad("unknown feature " + w[i] + " (feature schema mismatch)");
                m->features_.push_back(w[i]);
                m->feature_index_.push_back(static_cast<std::size_t>(it - names.begin()));
            }
        } else if (k == "mean" || k == "scale" || k == "ood_lo" || k == "ood_hi" || k == "w1" || k == "b1" ||
                   k == "w2" || k == "b2") {
            std::vector<double> v;
            if (w.size() < 2 || !numbers(w, v)) return bad("bad numbers for " + k);
            (k == "mean"     ? m->mean_
             : k == "scale"  ? m->scale_
             : k == "ood_lo" ? m->ood_lo_
             : k == "ood_hi" ? m->ood_hi_
             : k == "w1"     ? m->w1_
             : k == "b1"     ? m->b1_
             : k == "w2"     ? m->w2_
                             : m->b2_) = std::move(v);
        } else if (k == "ood_score_max" && w.size() == 2) {
            auto v = parse_double(w[1]);
            if (!v || *v <= 0) return bad("ood_score_max must be > 0");
            m->ood_score_max_ = *v;
        } else if (k == "hidden" && w.size() == 2) {
            auto v = parse_double(w[1]);
            if (!v || *v < 1 || *v > static_cast<double>(wm_limits::max_hidden) || std::floor(*v) != *v)
                return bad("hidden must be 1.." + std::to_string(wm_limits::max_hidden));
            m->hidden_ = static_cast<std::size_t>(*v);
        } else if (k == "target" && w.size() == 6) {
            if (m->targets_.size() >= wm_limits::max_targets) return bad("too many targets");
            Target t;
            t.name = w[1];
            t.unit = w[2];
            t.transform = w[3];
            if (!valid_token(t.name) || !valid_token(t.unit) || (t.transform != "identity" && t.transform != "log1p"))
                return bad("bad target line");
            auto lo = parse_double(w[4]);
            if (!lo) return bad("bad target bound");
            t.lo = *lo;
            if (w[5] == "cap") {
                t.cap_bound = true;
                t.hi = 1e9;
            } else {
                auto hi = parse_double(w[5]);
                if (!hi || *hi <= *lo) return bad("bad target bound");
                t.hi = *hi;
            }
            for (const auto& o : m->targets_)
                if (o.name == t.name) return bad("duplicate target " + t.name);
            m->targets_.push_back(std::move(t));
        } else if (k == "conformal" && w.size() == 4) {
            auto lvl = parse_double(w[2]), hw = parse_double(w[3]);
            if (!lvl || *lvl <= 0 || *lvl >= 1 || !hw || *hw < 0) return bad("bad conformal line");
            conformal.push_back({w[1], {*lvl, *hw}});
        } else if (k == "conformal_regime" && w.size() == 6) {
            auto lvl = parse_double(w[2]), lo = parse_double(w[3]), hw = parse_double(w[5]);
            std::optional<double> hi = w[4] == "inf" ? std::optional<double>(1e300) : parse_double(w[4]);
            if (!lvl || *lvl <= 0 || *lvl >= 1 || !lo || !hi || *hi <= *lo || *lo < 0 || !hw || *hw < 0)
                return bad("bad conformal_regime line");
            if (regimes.size() >= 64) return bad("too many conformal_regime lines");
            regimes.push_back({w[1], {*lvl, *lo, *hi, *hw}});
        } else if (k == "abstain_width" && w.size() == 3) {
            auto v = parse_double(w[2]);
            if (!v || *v <= 0) return bad("bad abstain_width");
            abstain.emplace_back(w[1], *v);
        } else if (k == "action" && w.size() == 4) {
            auto kind = action_from_string(w[1]);
            auto lo = parse_double(w[2]), hi = parse_double(w[3]);
            if (!kind || !lo || !hi || *hi < *lo) return bad("bad action line");
            m->action_ranges_.push_back({*kind, {*lo, *hi}});
        } else if (k == "residual_on" && w.size() == 2 && w[1] == "mechanistic") {
            m->residual_on_mechanistic_ = true;
        } else if (k == "provenance" && w.size() >= 2) {
            // Printable ASCII only: shown to users verbatim.
            const auto rest = line.substr(line.find("provenance") + 10);
            if (rest.size() > 300 || !std::all_of(rest.begin(), rest.end(), [](char c) { return c >= 0x20 && c < 0x7f; }))
                return bad("provenance must be printable ASCII, <= 300 characters");
            m->provenance_ = rest.substr(rest.find_first_not_of(' '));
        } else if (k == "meta") {
            // free-form provenance (training data, date, metrics); ignored by inference
        } else if (k == "feature_schema" && w.size() == 2) {
            auto v = parse_double(w[1]);
            if (!v || std::floor(*v) != *v) return bad("bad feature_schema");
            // Compare as double: casting an out-of-range value to int is undefined (found by fuzzing).
            if (*v != static_cast<double>(kFeatureSchemaVersion))
                return bad("feature schema " + w[1] + " is not supported (this build: " +
                           std::to_string(kFeatureSchemaVersion) + ")");
            m->support_.feature_schema = static_cast<int>(*v);
        } else if ((k == "model_version" || k == "data_version" || k == "calibration_version") && w.size() == 2 &&
                   valid_token(w[1])) {
            (k == "model_version" ? m->support_.model_version
             : k == "data_version" ? m->support_.data_version
                                   : m->support_.calibration_version) = w[1];
        } else if (k == "support_arch" && w.size() >= 2 && w.size() <= 9) {
            for (std::size_t i = 1; i < w.size(); ++i) {
                if (!valid_token(w[i])) return bad("bad support_arch");
                m->support_.arch.push_back(w[i]);
            }
        } else if (k == "support_kernel" && w.size() == 3) {
            auto lo = kernel_version(w[1]), hi = kernel_version(w[2]);
            if (!lo || !hi || *hi < *lo) return bad("support_kernel must be 'MAJOR.MINOR MAJOR.MINOR'");
            m->support_.kernel_min = lo;
            m->support_.kernel_max = hi;
        } else if (k == "support_cgroup" && w.size() == 2 && w[1] == "2") {
            m->support_.cgroup_version = 2;
        } else if (k == "support_throttle_semantics" && w.size() == 2 && w[1] == "summed_per_cpu") {
            m->support_.throttle_semantics = w[1];
        } else if (k == "support_capacity" && w.size() == 3) {
            auto lo = parse_double(w[1]), hi = parse_double(w[2]);
            if (!lo || !hi || *lo < 0 || *hi <= *lo) return bad("bad support_capacity");
            m->support_.capacity = std::make_pair(*lo, *hi);
        } else if (k == "workload_coverage" && w.size() >= 2 && w.size() <= 33) {
            for (std::size_t i = 1; i < w.size(); ++i) {
                if (!valid_token(w[i])) return bad("bad workload_coverage");
                m->support_.workloads.push_back(w[i]);
            }
        } else if (k == "target_status" && w.size() == 3 && (w[2] == "validated" || w[2] == "experimental")) {
            statuses.emplace_back(w[1], w[2]);
        } else if (k == "residual_gate" && w.size() == 3) {
            auto v = parse_double(w[2]);
            if (!v || *v < 0) return bad("bad residual_gate");
            gates.emplace_back(w[1], *v);
        } else if (k == "safe_throttle_max" && w.size() == 2) {
            auto v = parse_double(w[1]);
            if (!v || *v < 0 || *v > 64) return bad("bad safe_throttle_max");
            m->safe_throttle_max_ = *v;
        } else {
            return bad("unknown or malformed line: " + k);
        }
    }
    if (!ended) return bad("missing 'end' (truncated file)");
    // ---- consistency -------------------------------------------------------
    const std::size_t n = m->features_.size(), T = m->targets_.size();
    if (m->id_.empty() || m->kind_.empty() || n == 0 || T == 0) return bad("incomplete header");
    if (m->mean_.size() != n || m->scale_.size() != n || m->ood_lo_.size() != n || m->ood_hi_.size() != n)
        return bad("feature statistics do not match the feature count");
    if (std::any_of(m->scale_.begin(), m->scale_.end(), [](double s) { return !(s > 0); })) return bad("scale must be > 0");
    if (m->ood_score_max_ <= 0) return bad("missing ood_score_max");
    if (m->kind_ == "mlp") {
        const std::size_t H = m->hidden_;
        if (H == 0 || m->w1_.size() != H * n || m->b1_.size() != H || m->w2_.size() != T * H || m->b2_.size() != T)
            return bad("MLP weight shapes do not match");
    } else {
        if (m->hidden_ != 0 || !m->w1_.empty() || m->w2_.size() != T * n || m->b2_.size() != T)
            return bad("linear weight shapes do not match");
    }
    for (auto& t : m->targets_) {
        for (const auto& [name, c] : conformal)
            if (name == t.name) t.conformal.push_back(c);
        for (const auto& [name, v] : abstain)
            if (name == t.name) t.abstain_width = v;
        if (t.conformal.empty()) return bad("target " + t.name + " has no calibrated interval");
        std::sort(t.conformal.begin(), t.conformal.end());
        for (const auto& [name, st] : statuses)
            if (name == t.name) t.status = st;
        for (const auto& [name, g] : gates)
            if (name == t.name) t.residual_gate = g;
        for (const auto& [name, r] : regimes) {
            if (name != t.name) continue;
            const double level = r[0];  // structured bindings cannot be captured by older compilers
            if (std::none_of(t.conformal.begin(), t.conformal.end(), [level](const auto& c) { return c.first == level; }))
                return bad("conformal_regime level without a global conformal line for " + t.name);
            t.conformal_regime.push_back(r);
        }
    }
    for (const auto& [name, r] : regimes)
        if (std::none_of(m->targets_.begin(), m->targets_.end(), [&](const Target& t) { return t.name == name; }))
            return bad("conformal_regime for unknown target " + name);
    for (const auto& [name, st] : statuses)
        if (std::none_of(m->targets_.begin(), m->targets_.end(), [&](const Target& t) { return t.name == name; }))
            return bad("target_status for unknown target " + name);
    for (const auto& [name, g] : gates)
        if (std::none_of(m->targets_.begin(), m->targets_.end(), [&](const Target& t) { return t.name == name; }))
            return bad("residual_gate for unknown target " + name);
    if (!gates.empty() && !m->residual_on_mechanistic_) return bad("residual_gate requires residual_on mechanistic");
    if (m->format_ >= 2) {
        const auto& s = m->support_;
        if (s.feature_schema == 0 || s.model_version.empty() || s.data_version.empty() || s.calibration_version.empty() ||
            s.arch.empty() || !s.kernel_min || s.cgroup_version == 0 || s.throttle_semantics.empty() || !s.capacity ||
            s.workloads.empty())
            return bad("OSIEWM2 requires feature_schema, model/data/calibration versions and every support_* line");
    }
    for (const auto& [name, c] : conformal)
        if (std::none_of(m->targets_.begin(), m->targets_.end(), [&](const Target& t) { return t.name == name; }))
            return bad("conformal line for unknown target " + name);
    if (m->action_ranges_.empty()) return bad("no supported actions");
    return m;
}

std::optional<std::pair<int, int>> kernel_version(std::string_view r) {
    int major = 0, minor = 0;
    std::size_t i = 0;
    auto num = [&](int& out) {
        const std::size_t b = i;
        while (i < r.size() && i - b < 5 && r[i] >= '0' && r[i] <= '9') out = out * 10 + (r[i++] - '0');
        return i > b;
    };
    if (!num(major) || i >= r.size() || r[i] != '.') return std::nullopt;
    ++i;
    if (!num(minor)) return std::nullopt;
    return std::make_pair(major, minor);
}

std::vector<std::string> WorldModel::check_environment(const RuntimeEnvironment& env) const {
    std::vector<std::string> out;
    if (format_ < 2) {
        out.emplace_back("the model file (" + id_ + ") predates compatibility metadata (OSIEWM1), so its validity on "
                         "this machine is unknown");
        return out;
    }
    const auto& s = support_;
    if (std::find(s.arch.begin(), s.arch.end(), env.arch) == s.arch.end()) {
        std::string list;
        for (const auto& a : s.arch) list += (list.empty() ? "" : ", ") + a;
        out.push_back("CPU architecture " + (env.arch.empty() ? std::string("unknown") : env.arch) +
                      " was not validated (validated: " + list + ")");
    }
    const auto kv = kernel_version(env.kernel_release);
    auto ver = [](const std::pair<int, int>& v) { return std::to_string(v.first) + "." + std::to_string(v.second); };
    if (!kv) {
        out.push_back("kernel version '" + env.kernel_release + "' could not be parsed");
    } else if ((s.kernel_min && *kv < *s.kernel_min) || (s.kernel_max && *kv > *s.kernel_max)) {
        out.push_back("kernel " + ver(*kv) + " is outside the validated range " + ver(*s.kernel_min) + "-" +
                      ver(*s.kernel_max) + " (cgroup CPU accounting semantics may differ)");
    }
    if (env.cgroup_version != s.cgroup_version)
        out.push_back("cgroup v" + std::to_string(env.cgroup_version) + " is not supported (validated: cgroup v" +
                      std::to_string(s.cgroup_version) + ")");
    return out;
}

Result<WorldPrediction> IWorldModel::predict_in(const std::vector<CgroupWindow>& history, const Intervention& action,
                                                const RuntimeEnvironment& env) const {
    auto p = predict(history, action);
    if (!p) return p;
    p->environment_issues = check_environment(env);
    if (!p->environment_issues.empty()) {
        p->abstained = true;
        for (const auto& r : p->environment_issues) p->reasons.push_back(r);
        p->throttling_assessment = p->throttling_assessment.empty() ? "" : "unknown";
    }
    return p;
}

bool WorldModel::supports(ActionKind k) const noexcept {
    return std::any_of(action_ranges_.begin(), action_ranges_.end(), [k](const auto& r) { return r.first == k; });
}

std::size_t WorldModel::parameter_count() const noexcept { return w1_.size() + b1_.size() + w2_.size() + b2_.size(); }

std::vector<double> WorldModel::forward(const std::vector<double>& z) const {
    const std::size_t n = z.size(), T = targets_.size();
    std::vector<double> y(T);
    if (kind_ == "mlp") {
        std::vector<double> h(hidden_);
        for (std::size_t j = 0; j < hidden_; ++j) {
            double a = b1_[j];
            for (std::size_t i = 0; i < n; ++i) a += w1_[j * n + i] * z[i];
            h[j] = std::max(0.0, a);
        }
        for (std::size_t t = 0; t < T; ++t) {
            double a = b2_[t];
            for (std::size_t j = 0; j < hidden_; ++j) a += w2_[t * hidden_ + j] * h[j];
            y[t] = a;
        }
    } else {
        for (std::size_t t = 0; t < T; ++t) {
            double a = b2_[t];
            for (std::size_t i = 0; i < n; ++i) a += w2_[t * n + i] * z[i];
            y[t] = a;
        }
    }
    return y;
}

Result<WorldPrediction> WorldModel::predict(const std::vector<CgroupWindow>& history_in, const Intervention& a) const {
    if (history_in.empty()) return make_error(Errc::unavailable, "no observation windows");
    if (!supports(a.kind))
        return make_error(Errc::unsupported, "this model was not trained for '" + std::string(to_string(a.kind)) + "' interventions");
    // Use the most recent windows, as in training.
    const std::size_t take = std::min(history_, history_in.size());
    const std::vector<CgroupWindow> hist(history_in.end() - static_cast<std::ptrdiff_t>(take), history_in.end());
    auto enc = encode(hist, a);
    if (!enc) return enc.error();

    WorldPrediction p;
    p.action = a;
    p.model_id = id_;
    p.history_windows = hist.size();
    p.missing = enc->missing;
    if (hist.size() < history_) {
        p.out_of_distribution = true;
        p.reasons.push_back("only " + std::to_string(hist.size()) + " of the " + std::to_string(history_) +
                            " observation windows the model was trained with");
    }
    if (!p.missing.empty()) {
        p.out_of_distribution = true;
        p.reasons.push_back("required telemetry unavailable");
    }
    for (const auto& [kind, range] : action_ranges_) {
        if (kind != a.kind || kind == ActionKind::none) continue;
        for (double v : {a.previous, a.proposed}) {
            if (v < range.first - 1e-9 || v > range.second + 1e-9) {
                p.out_of_distribution = true;
                p.reasons.push_back(std::string(to_string(kind)) + " value " + std::to_string(v) +
                                    " is outside the validated range " + std::to_string(range.first) + "-" +
                                    std::to_string(range.second));
            }
        }
    }
    std::vector<double> z(features_.size());
    double score = 0;
    for (std::size_t i = 0; i < features_.size(); ++i) {
        const double x = enc->values[feature_index_[i]];
        if (x < ood_lo_[i] || x > ood_hi_[i]) {
            p.out_of_distribution = true;
            p.reasons.push_back("feature " + features_[i] + " = " + std::to_string(x) + " is outside the training range");
        }
        z[i] = (x - mean_[i]) / scale_[i];
        score += z[i] * z[i];
    }
    score /= static_cast<double>(features_.size());
    if (score > ood_score_max_) {
        p.out_of_distribution = true;
        p.reasons.push_back("the observed state is unlike the training data (distance score " + std::to_string(score) + ")");
    }

    const double cap = capacity_after(hist.back(), a);
    if (support_.capacity && (cap < support_.capacity->first - 1e-9 || cap > support_.capacity->second + 1e-9)) {
        p.out_of_distribution = true;
        p.reasons.push_back("CPU capacity after the change (" + std::to_string(cap) +
                            " cores) is outside the validated range");
    }
    // The demand estimate relies on throttled_usec being summed over CPUs
    // (validated semantics). Counters that cannot fit those semantics mean
    // the estimate would be wrong, so the prediction is withheld.
    if (support_.throttle_semantics == "summed_per_cpu") {
        for (const auto& w : hist) {
            const double tf = w.throttled_frac.value_or(0.0);
            const double cpus = w.cpuset_cpus.value_or(w.host_cpus.value_or(1024.0));
            const bool exceeds = tf > cpus * 1.05 + 0.05;
            const bool unexplained = w.throttled_ratio && *w.throttled_ratio == 0.0 && tf > 0.05;
            if (exceeds || unexplained) {
                p.out_of_distribution = true;
                p.reasons.push_back("cgroup throttling counters are inconsistent with the validated accounting "
                                    "semantics (throttled time without throttled periods, or more than one CPU per CPU)");
                break;
            }
        }
    }
    const auto raw = forward(z);
    for (std::size_t t = 0; t < targets_.size(); ++t) {
        const auto& tg = targets_[t];
        TargetPrediction tp;
        tp.name = tg.name;
        tp.unit = tg.unit;
        tp.current = current_value(tg.name, hist);
        double v = tg.transform == "log1p" ? std::expm1(raw[t]) : raw[t];
        if (residual_on_mechanistic_) {
            const auto prior = mechanistic_prior(tg.name, enc->values);
            if (!prior) return make_error(Errc::invalid_argument, "no mechanistic prior for target " + tg.name);
            // Gated residual: the learned correction is used only for states
            // like the training data, and only when it is a small correction.
            const bool use = !tg.residual_gate || (!p.out_of_distribution && std::fabs(v) <= *tg.residual_gate);
            tp.residual_applied = use && v != 0.0;
            v = use ? v + *prior : *prior;
        }
        tp.status = tg.status;
        const double hi = tg.cap_bound ? cap : tg.hi;
        if (!std::isfinite(v)) v = tg.lo;
        if (v < tg.lo || v > hi) tp.clamped = true;
        tp.expected = std::clamp(v, tg.lo, std::max(tg.lo, hi));
        // Measured usage can exceed the configured capacity slightly (period
        // alignment and accounting granularity: up to 2.2% in the development
        // data), so intervals for capacity-bounded targets allow 3% above it.
        const double iv_hi = tg.cap_bound ? cap * 1.03 : hi;
        // Regime for Mondrian intervals: demand estimate / capacity after the change.
        const auto& names = feature_names();
        const auto fi = [&](std::string_view n) {
            return enc->values[static_cast<std::size_t>(std::find(names.begin(), names.end(), n) - names.begin())];
        };
        const double rho = fi("m_demand_after") / std::max(1e-6, fi("m_cap_after"));
        for (auto [level, hw] : tg.conformal) {
            for (const auto& r : tg.conformal_regime)
                if (r[0] == level && rho >= r[1] && rho < r[2]) hw = r[3];
            tp.intervals.push_back(Interval{level, std::clamp(tp.expected - hw, tg.lo, std::max(tg.lo, iv_hi)),
                                            std::clamp(tp.expected + hw, tg.lo, std::max(tg.lo, iv_hi))});
        }
        // The usefulness gate uses the primary (lowest-level) interval. Split-
        // conformal widths do not depend on the state, so this is a check on the
        // model as a whole; per-case abstention comes from the OOD checks above.
        // The model-wide (not regime-conditional) primary width is used, so
        // regime calibration cannot turn "throttling likely" into an abstention.
        const double primary = 2.0 * tg.conformal.front().second;
        if (tg.abstain_width && primary > *tg.abstain_width) {
            p.abstained = true;
            p.reasons.push_back("the interval for " + tg.name + " is too wide to be useful");
        }
        p.targets.push_back(std::move(tp));
    }
    if (p.out_of_distribution) p.abstained = true;
    if (a.kind == ActionKind::cpu_quota || a.kind == ActionKind::cpuset) {
        const auto it = std::find_if(p.targets.begin(), p.targets.end(),
                                     [](const TargetPrediction& t) { return t.name == "throttled_cores"; });
        if (p.abstained || it == p.targets.end() || it->intervals.empty() || !safe_throttle_max_) {
            p.throttling_assessment = "unknown";
        } else {
            const auto& widest = it->intervals.back();
            p.throttling_assessment = widest.hi <= *safe_throttle_max_ ? "low"
                                      : widest.lo > *safe_throttle_max_ ? "likely"
                                                                        : "possible";
        }
    }
    return p;
}

}  // namespace osie::predict
