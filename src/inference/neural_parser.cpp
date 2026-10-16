// SPDX-License-Identifier: Apache-2.0
#include "osie/inference/neural_parser.hpp"

#include "osie/inference/model_locator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace osie::inference {
namespace {

char shape_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return 'X';
    if (c >= 'a' && c <= 'z') return 'x';
    if (c >= '0' && c <= '9') return 'd';
    if (c >= 0x80) return 'u';
    return static_cast<char>(c);
}

inline float sigmoid(float x) { return 1.0F / (1.0F + std::exp(-x)); }

void softmax(float* v, std::size_t n) {
    // Defence in depth: the loader bounds weights so logits stay finite, but a
    // non-finite logit must still yield a valid (uniform) distribution.
    if (!std::all_of(v, v + n, [](float x) { return std::isfinite(x); })) {
        std::fill(v, v + n, 1.0F / static_cast<float>(n));
        return;
    }
    float mx = v[0];
    for (std::size_t i = 1; i < n; ++i) mx = std::max(mx, v[i]);
    float sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = std::exp(v[i] - mx);
        sum += v[i];
    }
    for (std::size_t i = 0; i < n; ++i) v[i] /= sum;
}

/// y[o] = b[o] + sum_i W[o, i] * x[i]   (W row-major [out, in])
void linear(const float* W, const float* b, const float* x, float* y, std::size_t out, std::size_t in) {
    for (std::size_t o = 0; o < out; ++o) {
        const float* w = W + o * in;
        float acc = b != nullptr ? b[o] : 0.0F;
        for (std::size_t i = 0; i < in; ++i) acc += w[i] * x[i];
        y[o] = acc;
    }
}

/// One GRU direction over x [T, D] -> h [T, H] (PyTorch gate order r, z, n).
void gru_dir(const Tensor& wih, const Tensor& whh, const Tensor& bih, const Tensor& bhh, const std::vector<float>& x,
             std::size_t T, std::size_t D, std::size_t H, bool reverse, float* out, std::size_t out_stride,
             std::size_t out_off) {
    std::vector<float> h(H, 0.0F), gi(3 * H), gh(3 * H);
    for (std::size_t step = 0; step < T; ++step) {
        const std::size_t t = reverse ? T - 1 - step : step;
        linear(wih.f32.data(), bih.f32.data(), &x[t * D], gi.data(), 3 * H, D);
        linear(whh.f32.data(), bhh.f32.data(), h.data(), gh.data(), 3 * H, H);
        for (std::size_t j = 0; j < H; ++j) {
            const float r = sigmoid(gi[j] + gh[j]);
            const float z = sigmoid(gi[H + j] + gh[H + j]);
            const float n = std::tanh(gi[2 * H + j] + r * gh[2 * H + j]);
            h[j] = (1.0F - z) * n + z * h[j];
        }
        std::memcpy(out + t * out_stride + out_off, h.data(), H * sizeof(float));
    }
}

/// Same-padded 1-D convolution, kernel k, weight [Cout, Cin, k], ReLU.
void conv1d_relu(const Tensor& w, const Tensor& b, const std::vector<float>& x, std::size_t T, std::size_t Cin,
                 std::vector<float>& y) {
    const std::size_t Cout = w.shape[0];
    const std::size_t k = w.shape[2];
    const std::ptrdiff_t pad = static_cast<std::ptrdiff_t>(k / 2);
    y.assign(T * Cout, 0.0F);
    for (std::size_t t = 0; t < T; ++t) {
        for (std::size_t o = 0; o < Cout; ++o) {
            float acc = b.f32[o];
            for (std::size_t j = 0; j < k; ++j) {
                const std::ptrdiff_t src = static_cast<std::ptrdiff_t>(t) + static_cast<std::ptrdiff_t>(j) - pad;
                if (src < 0 || src >= static_cast<std::ptrdiff_t>(T)) continue;
                const float* xi = &x[static_cast<std::size_t>(src) * Cin];
                const float* wi = &w.f32[(o * Cin) * k];
                for (std::size_t c = 0; c < Cin; ++c) acc += wi[c * k + j] * xi[c];
            }
            y[t * Cout + o] = std::max(acc, 0.0F);
        }
    }
}

std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> v;
    for (std::string w; is >> w;) v.push_back(w);
    return v;
}

}  // namespace

std::string_view knowledge_category_list(std::string_view lower_token) {
    // OS-domain lexicon categories ("knowledge-assisted" features). Must match
    // KNOWLEDGE in ml/osie_ml/text.py exactly (checked by check_parity.py).
    struct Cat {
        std::string_view name;
        std::initializer_list<std::string_view> words;
    };
    static const Cat cats[] = {
        {"cpu", {"cpu", "cpus", "processor", "processors", "core", "cores", "load", "cycles"}},
        {"mem", {"memory", "ram", "mem", "swap", "swapping", "paging", "footprint", "leak", "leaking"}},
        {"disk", {"disk", "disks", "storage", "drive", "ssd", "volume", "volumes", "filesystem", "filesystems", "space"}},
        {"net", {"network", "wifi", "ethernet", "interface", "interfaces", "bandwidth", "internet", "traffic", "ip", "packets"}},
        {"heat", {"thermal", "temperature", "overheating", "overheat", "hot", "heat", "fan", "fans", "warm"}},
        {"why", {"why", "explain", "cause", "causing", "reason", "diagnose", "investigate", "troubleshoot", "wrong"}},
        {"slow", {"slow", "sluggish", "laggy", "lag", "lagging", "unresponsive", "freezing", "frozen", "beachball", "hang",
                  "hanging", "stutter", "crawling", "performance"}},
        {"use", {"using", "uses", "use", "consuming", "consume", "hogging", "hog", "eating", "eat", "taking", "burning", "chewing"}},
        {"top", {"top", "most", "biggest", "highest", "heaviest", "largest", "busiest", "worst"}},
        {"act", {"kill", "terminate", "quit", "restart", "reboot", "renice", "stop", "close", "set", "change", "increase",
                 "decrease", "lower", "raise", "pin", "disable", "enable", "clear", "free", "optimize"}},
        {"k8s", {"kubernetes", "k8s", "pod", "pods", "kubectl", "deployment", "kubelet", "replicas"}},
        {"ctr", {"container", "containers", "docker", "cgroup", "cgroups"}},
        {"thr", {"throttled", "throttling", "throttle", "quota", "limit", "limited"}},
        {"tree", {"parent", "child", "children", "spawned", "spawn", "started", "launched", "owns", "owner", "tree", "service",
                  "services", "daemon"}},
        {"sys", {"os", "macos", "linux", "version", "kernel", "uptime", "hostname", "model", "distro", "boot", "booted"}},
        {"ref", {"it", "that", "this", "these", "those", "them", "one"}},
        {"q", {"what", "which", "who", "how", "where", "when"}},
        {"pid", {"pid", "process"}},
        {"help", {"help", "hello", "hi", "hey", "commands"}},
        {"ifq", {"if", "happen", "happens", "would"}},
    };
    for (const auto& c : cats)
        for (auto w : c.words)
            if (w == lower_token) return c.name;
    return {};
}

std::vector<std::uint32_t> token_features(std::string_view tok, std::uint32_t buckets, int nmin, int nmax,
                                          bool knowledge) {
    std::string low(tok);
    for (auto& c : low)
        if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<std::uint32_t> f;
    f.reserve(32);
    f.push_back(fnv1a("w:" + low) % buckets);
    const std::string s = "<" + low.substr(0, 24) + ">";
    std::string key;
    for (int n = nmin; n <= nmax; ++n) {
        if (static_cast<std::size_t>(n) > s.size()) break;
        for (std::size_t i = 0; i + static_cast<std::size_t>(n) <= s.size(); ++i) {
            key = "c" + std::to_string(n) + ":";
            key.append(s, i, static_cast<std::size_t>(n));
            f.push_back(fnv1a(key) % buckets);
        }
    }
    std::string shape;
    for (char c : tok) {
        const char sc = shape_char(static_cast<unsigned char>(c));
        if (shape.empty() || shape.back() != sc) shape += sc;
        if (shape.size() >= 8) break;
    }
    f.push_back(fnv1a("s:" + shape) % buckets);
    if (knowledge) {
        const auto cat = knowledge_category_list(low);
        if (!cat.empty()) f.push_back(fnv1a("k:" + std::string(cat)) % buckets);
    }
    return f;
}

Result<IntentModel> IntentModel::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return make_error(Errc::not_found, "cannot open model " + path);
    // Size check before reading: a huge or special file must not be slurped.
    const std::streamoff size = in.tellg();
    if (size < 0) return make_error(Errc::io_error, "cannot determine size of " + path);
    if (static_cast<std::uint64_t>(size) > limits::max_file_bytes)
        return make_error(Errc::parse_error, "model file exceeds " + std::to_string(limits::max_file_bytes) + " bytes");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    if (!in.read(bytes.data(), size)) return make_error(Errc::io_error, "cannot read model " + path);
    return load_from_memory(bytes);
}

namespace {

/// Strict unsigned decimal: digits only, whole token, no sign, no overflow.
std::optional<std::uint64_t> parse_u64(std::string_view s, std::uint64_t max) {
    if (s.empty() || s.size() > 19) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > max) return std::nullopt;
    return v;
}

/// a * b, or nullopt on overflow or when the product exceeds `max`.
std::optional<std::size_t> checked_mul(std::size_t a, std::size_t b, std::size_t max) {
    if (a != 0 && b > max / a) return std::nullopt;
    return a * b;
}

/// Labels and names are restricted to a printable ASCII subset so that
/// malformed metadata cannot inject control characters or invalid UTF-8
/// into terminal output or JSON.
bool valid_label(std::string_view s) {
    if (s.empty() || s.size() > limits::max_label_chars) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

/// Finite and |x| <= limits::max_abs_weight. Finite alone is not enough:
/// weights near FLT_MAX overflow to inf/NaN in the forward pass (found by
/// fuzzing). The bound keeps every activation of the supported architectures
/// finite (see docs/MODEL_FORMAT.md).
bool all_bounded(const std::vector<float>& v) {
    return std::all_of(v.begin(), v.end(), [](float x) {
        return std::isfinite(x) && std::fabs(x) <= limits::max_abs_weight;
    });
}

}  // namespace

Result<IntentModel> IntentModel::load_from_memory(std::string_view bytes) {
    try {
        return parse_model(bytes);
    } catch (const std::exception& e) {
        // parse_model checks every size before allocating; this is a last line of defence.
        return make_error(Errc::parse_error, std::string("model rejected: ") + e.what());
    }
}

Result<IntentModel> IntentModel::parse_model(std::string_view bytes) {
    if (bytes.size() > limits::max_file_bytes) return make_error(Errc::parse_error, "model exceeds size limit");
    IntentModel m;
    std::size_t pos = 0;
    std::size_t header_lines = 0;
    auto next_line = [&](std::string_view& line) {
        const auto e = bytes.find('\n', pos);
        if (e == std::string_view::npos) return false;
        line = bytes.substr(pos, e - pos);
        pos = e + 1;
        return true;
    };
    auto bad = [](std::string msg) { return make_error(Errc::parse_error, std::move(msg)); };
    std::string_view line;
    if (!next_line(line) || line != "OSIEM1") return bad("bad magic (expected OSIEM1)");
    struct Pending {
        std::string name, dtype;
        std::vector<std::size_t> shape;
        std::size_t elements{0};
    };
    std::vector<Pending> pending;
    std::set<std::string, std::less<>> seen_keys, seen_tensors;
    bool have_data = false;
    while (next_line(line)) {
        if (line == "data") {
            have_data = true;
            break;
        }
        if (++header_lines > limits::max_header_lines) return bad("too many header lines");
        if (pos > limits::max_header_bytes) return bad("header too large");
        auto w = split_ws(std::string(line));
        if (w.empty()) continue;
        const auto& k = w[0];
        if (k != "tensor" && !seen_keys.insert(k).second) return bad("duplicate header key " + k);
        auto num = [&](std::size_t i, std::uint64_t max) { return parse_u64(w[i], max); };
        if (k == "arch" && w.size() == 2) {
            m.arch_ = w[1];
        } else if (k == "buckets" && w.size() == 2) {
            auto v = num(1, limits::max_buckets);
            if (!v || *v == 0) return bad("buckets must be 1.." + std::to_string(limits::max_buckets));
            m.buckets_ = static_cast<std::uint32_t>(*v);
        } else if ((k == "dim" || k == "hidden") && w.size() == 2) {
            auto v = num(1, limits::max_width);
            if (!v || *v == 0) return bad(k + " must be 1.." + std::to_string(limits::max_width));
            (k == "dim" ? m.dim_ : m.hidden_) = static_cast<std::size_t>(*v);
        } else if (k == "ngram" && w.size() == 3) {
            auto a = num(1, limits::max_ngram), b = num(2, limits::max_ngram);
            if (!a || !b || *a == 0 || *a > *b) return bad("ngram must satisfy 1 <= min <= max <= " + std::to_string(limits::max_ngram));
            m.ngram_min_ = static_cast<int>(*a);
            m.ngram_max_ = static_cast<int>(*b);
        } else if (k == "knowledge" && w.size() == 2) {
            if (w[1] != "0" && w[1] != "1") return bad("knowledge must be 0 or 1");
            m.knowledge_ = w[1] == "1";
        } else if (k == "hybrid_threshold" && w.size() == 2) {
            char* end = nullptr;
            const float v = std::strtof(w[1].c_str(), &end);
            if (end != w[1].c_str() + w[1].size() || !std::isfinite(v) || v < 0.0F || v > 1.0F)
                return bad("hybrid_threshold must be a finite number in [0,1]");
            m.hybrid_threshold_ = v;
        } else if (k == "intents" || k == "tags") {
            auto& dst = k == "intents" ? m.intents_ : m.tags_;
            const std::size_t max = k == "intents" ? limits::max_intents : limits::max_tags;
            if (w.size() < 2 || w.size() - 1 > max) return bad(k + " must list 1.." + std::to_string(max) + " labels");
            std::set<std::string, std::less<>> uniq;
            for (std::size_t i = 1; i < w.size(); ++i) {
                if (!valid_label(w[i])) return bad("invalid label in " + k);
                if (!uniq.insert(w[i]).second) return bad("duplicate label " + w[i] + " in " + k);
            }
            dst.assign(w.begin() + 1, w.end());
        } else if (k == "tensor" && w.size() >= 4 && w.size() <= 3 + limits::max_rank) {
            if (pending.size() >= limits::max_tensors) return bad("too many tensors");
            if (!valid_label(w[1])) return bad("invalid tensor name");
            if (!seen_tensors.insert(w[1]).second) return bad("duplicate tensor " + w[1]);
            Pending p{w[1], w[2], {}, 1};
            for (std::size_t i = 3; i < w.size(); ++i) {
                auto d = num(i, limits::max_dim);
                if (!d || *d == 0) return bad("bad shape for " + p.name);
                p.shape.push_back(static_cast<std::size_t>(*d));
                auto n = checked_mul(p.elements, p.shape.back(), limits::max_tensor_elements);
                if (!n) return bad("tensor " + p.name + " exceeds " + std::to_string(limits::max_tensor_elements) + " elements");
                p.elements = *n;
            }
            pending.push_back(std::move(p));
        } else if (k == "arch" || k == "buckets" || k == "dim" || k == "hidden" || k == "ngram" || k == "knowledge" ||
                   k == "hybrid_threshold" || k == "tensor") {
            return bad("malformed header line: " + k);
        } else if (!valid_label(k)) {
            return bad("invalid header key");
        }
        // Other well-formed keys are optional metadata from newer writers and are ignored.
    }
    if (!have_data) return bad("missing data section");
    const std::size_t data_start = pos;  // int8 padding is relative to the data section
    if (m.arch_ != "bigru" && m.arch_ != "cnn" && m.arch_ != "bag") return bad("unknown arch");
    if (m.buckets_ == 0 || m.dim_ == 0 || m.hidden_ == 0 || m.intents_.empty() || m.tags_.empty())
        return bad("incomplete header");

    // Every size below is bounded by max_tensor_elements, so the arithmetic
    // cannot overflow; reads are checked against the remaining bytes before
    // anything is allocated.
    auto remaining = [&]() { return bytes.size() - pos; };
    for (const auto& p : pending) {
        Tensor t;
        t.shape = p.shape;
        const std::size_t n = p.elements;
        if (p.dtype == "f32") {
            if (n * 4 > remaining()) return bad("truncated tensor " + p.name);
            t.f32.resize(n);
            std::memcpy(t.f32.data(), bytes.data() + pos, n * 4);
            pos += n * 4;
            if (!all_bounded(t.f32)) return bad("non-finite or out-of-range value in tensor " + p.name);
        } else if (p.dtype == "i8") {
            const std::size_t rows = p.shape[0];
            if (rows * 4 > remaining() || n > remaining() - rows * 4) return bad("truncated tensor " + p.name);
            t.row_scale.resize(rows);
            std::memcpy(t.row_scale.data(), bytes.data() + pos, rows * 4);
            pos += rows * 4;
            if (!all_bounded(t.row_scale)) return bad("non-finite or out-of-range scale in tensor " + p.name);
            t.i8.resize(n);
            std::memcpy(t.i8.data(), bytes.data() + pos, n);
            pos += n;
            const std::size_t aligned = data_start + ((pos - data_start + 3U) & ~std::size_t{3});  // 4-byte alignment
            pos = std::min(aligned, bytes.size());
        } else {
            return bad("unknown dtype " + p.dtype);
        }
        m.tensors_.emplace(p.name, std::move(t));
    }
    if (pos != bytes.size()) return bad(std::to_string(bytes.size() - pos) + " unexpected trailing bytes");
    auto need = [&m](const std::string& n, const std::vector<std::size_t>& shape) -> Result<void> {
        auto it = m.tensors_.find(n);
        if (it == m.tensors_.end()) return make_error(Errc::parse_error, "missing tensor " + n);
        if (it->second.shape != shape) return make_error(Errc::parse_error, "shape mismatch for " + n);
        return {};
    };
    const std::size_t D = m.dim_, H = m.hidden_, NI = m.intents_.size(), NT = m.tags_.size();
    std::vector<std::pair<std::string, std::vector<std::size_t>>> req = {{"emb", {m.buckets_, D}}};
    std::size_t enc = 0;
    if (m.arch_ == "bigru") {
        for (const char* d : {"f", "b"}) {
            req.push_back({std::string("gru_") + d + "_wih", {3 * H, D}});
            req.push_back({std::string("gru_") + d + "_whh", {3 * H, H}});
            req.push_back({std::string("gru_") + d + "_bih", {3 * H}});
            req.push_back({std::string("gru_") + d + "_bhh", {3 * H}});
        }
        enc = 2 * H;
    } else if (m.arch_ == "cnn") {
        req.push_back({"conv1_w", {H, D, 3}});
        req.push_back({"conv1_b", {H}});
        req.push_back({"conv2_w", {H, H, 3}});
        req.push_back({"conv2_b", {H}});
        enc = H;
    } else {
        req.push_back({"proj_w", {H, D}});
        req.push_back({"proj_b", {H}});
        enc = H;
    }
    req.push_back({"intent_w", {NI, 2 * enc}});
    req.push_back({"intent_b", {NI}});
    req.push_back({"tag_w", {NT, enc}});
    req.push_back({"tag_b", {NT}});
    for (const auto& [n, s] : req)
        if (auto r = need(n, s); !r) return r.error();
    for (const auto& [name, ten] : m.tensors_)
        if (ten.quantized() && name != "emb") return make_error(Errc::parse_error, "only emb may be int8");
    if (m.tensors_.size() != req.size()) return make_error(Errc::parse_error, "unexpected extra tensors for arch " + m.arch_);

    m.intent_map_.clear();
    for (const auto& l : m.intents_) {
        auto i = intent::intent_from_string(l);
        m.intent_map_.push_back(i ? static_cast<int>(*i) : -1);
    }
    return m;
}

const Tensor& IntentModel::t(const std::string& name) const { return tensors_.at(name); }

void IntentModel::embed(const std::vector<intent::Token>& toks, std::vector<float>& x) const {
    const auto& E = t("emb");
    const std::size_t D = dim_;
    x.assign(toks.size() * D, 0.0F);
    for (std::size_t i = 0; i < toks.size(); ++i) {
        const auto feats = token_features(toks[i].text, buckets_, ngram_min_, ngram_max_, knowledge_);
        float* xi = &x[i * D];
        for (auto id : feats) {
            if (E.quantized()) {
                const float s = E.row_scale[id];
                const std::int8_t* row = &E.i8[static_cast<std::size_t>(id) * D];
                for (std::size_t d = 0; d < D; ++d) xi[d] += s * static_cast<float>(row[d]);
            } else {
                const float* row = &E.f32[static_cast<std::size_t>(id) * D];
                for (std::size_t d = 0; d < D; ++d) xi[d] += row[d];
            }
        }
        const float inv = 1.0F / static_cast<float>(feats.size());
        for (std::size_t d = 0; d < D; ++d) xi[d] *= inv;
    }
}

ModelOutput IntentModel::forward(const std::vector<intent::Token>& toks) const {
    ModelOutput out;
    const std::size_t NI = intents_.size(), NT = tags_.size();
    out.intent_probs.assign(NI, 1.0F / static_cast<float>(NI));
    if (toks.empty()) return out;
    const std::size_t T = toks.size(), D = dim_, H = hidden_;
    std::vector<float> x;
    embed(toks, x);

    std::vector<float> h;
    std::size_t enc = 0;
    if (arch_ == "bigru") {
        enc = 2 * H;
        h.assign(T * enc, 0.0F);
        gru_dir(t("gru_f_wih"), t("gru_f_whh"), t("gru_f_bih"), t("gru_f_bhh"), x, T, D, H, false, h.data(), enc, 0);
        gru_dir(t("gru_b_wih"), t("gru_b_whh"), t("gru_b_bih"), t("gru_b_bhh"), x, T, D, H, true, h.data(), enc, H);
    } else if (arch_ == "cnn") {
        enc = H;
        std::vector<float> h1;
        conv1d_relu(t("conv1_w"), t("conv1_b"), x, T, D, h1);
        conv1d_relu(t("conv2_w"), t("conv2_b"), h1, T, H, h);
    } else {
        enc = H;
        h.assign(T * H, 0.0F);
        const auto& W = t("proj_w");
        const auto& b = t("proj_b");
        for (std::size_t i = 0; i < T; ++i) {
            linear(W.f32.data(), b.f32.data(), &x[i * D], &h[i * H], H, D);
            for (std::size_t j = 0; j < H; ++j) h[i * H + j] = std::max(h[i * H + j], 0.0F);
        }
    }
    // Sentence representation: [mean ; max] over time.
    std::vector<float> pooled(2 * enc, 0.0F);
    for (std::size_t j = 0; j < enc; ++j) pooled[enc + j] = -1e30F;
    for (std::size_t i = 0; i < T; ++i) {
        for (std::size_t j = 0; j < enc; ++j) {
            const float v = h[i * enc + j];
            pooled[j] += v;
            pooled[enc + j] = std::max(pooled[enc + j], v);
        }
    }
    for (std::size_t j = 0; j < enc; ++j) pooled[j] /= static_cast<float>(T);
    linear(t("intent_w").f32.data(), t("intent_b").f32.data(), pooled.data(), out.intent_probs.data(), NI, 2 * enc);
    softmax(out.intent_probs.data(), NI);

    out.tag_probs.assign(T, std::vector<float>(NT));
    for (std::size_t i = 0; i < T; ++i) {
        linear(t("tag_w").f32.data(), t("tag_b").f32.data(), &h[i * enc], out.tag_probs[i].data(), NT, enc);
        softmax(out.tag_probs[i].data(), NT);
    }
    return out;
}

intent::ParseOutput IntentModel::parse(const std::vector<intent::Token>& toks) const {
    intent::ParseOutput p;
    p.parser = "neural";
    const auto out = forward(toks);
    const auto best = static_cast<std::size_t>(
        std::max_element(out.intent_probs.begin(), out.intent_probs.end()) - out.intent_probs.begin());
    p.confidence = out.intent_probs[best];
    p.intent = intent_map_[best] >= 0 ? static_cast<intent::Intent>(intent_map_[best]) : intent::Intent::unsupported;

    // BIO decode. Tag labels look like "O", "B-PROC", "I-PROC", ...
    auto type_of = [](std::string_view s) -> std::optional<intent::SpanType> {
        if (s == "PROC") return intent::SpanType::proc;
        if (s == "PID") return intent::SpanType::pid;
        if (s == "REF") return intent::SpanType::ref;
        if (s == "NUM") return intent::SpanType::num;
        return std::nullopt;
    };
    std::optional<intent::Span> cur;
    for (std::size_t i = 0; i < out.tag_probs.size(); ++i) {
        const auto& tp = out.tag_probs[i];
        const auto k = static_cast<std::size_t>(std::max_element(tp.begin(), tp.end()) - tp.begin());
        const std::string& lab = tags_[k];
        std::optional<intent::SpanType> ty;
        bool inside = false;
        if (lab.size() > 2 && lab[1] == '-') {
            ty = type_of(std::string_view(lab).substr(2));
            inside = lab[0] == 'I';
        }
        if (!ty) {
            if (cur) p.spans.push_back(*cur);
            cur.reset();
            continue;
        }
        if (inside && cur && cur->type == *ty) {
            cur->end = i + 1;  // continue span
        } else {               // B-, or I- without a matching open span (repair)
            if (cur) p.spans.push_back(*cur);
            cur = intent::Span{*ty, i, i + 1};
        }
    }
    if (cur) p.spans.push_back(*cur);
    return p;
}

std::size_t IntentModel::parameter_count() const {
    std::size_t n = 0;
    for (const auto& [_, t] : tensors_) {
        std::size_t k = 1;
        for (auto d : t.shape) k *= d;
        n += k;
    }
    return n;
}

std::size_t IntentModel::weight_bytes() const {
    std::size_t n = 0;
    for (const auto& [_, t] : tensors_) n += t.f32.size() * 4 + t.i8.size() + t.row_scale.size() * 4;
    return n;
}

intent::ParseOutput HybridParser::parse(const std::vector<intent::Token>& toks) const {
    auto n = m_->parse(toks);
    if (n.confidence < threshold_) {
        auto r = rules_.parse(toks);
        r.parser = "hybrid:rules";
        return r;
    }
    n.parser = "hybrid:neural";
    const bool process_intent = n.intent == intent::Intent::process_cpu || n.intent == intent::Intent::process_memory ||
                                n.intent == intent::Intent::process_info || n.intent == intent::Intent::process_tree;
    const bool has_target = std::any_of(n.spans.begin(), n.spans.end(), [](const intent::Span& s) {
        return s.type != intent::SpanType::num;
    });
    if (process_intent && !has_target) {
        for (const auto& s : intent::RuleParser::extract_spans(toks)) n.spans.push_back(s);
        n.parser = "hybrid:neural+rule_slots";
    }
    return n;
}

std::unique_ptr<intent::IIntentParser> make_parser(ParserChoice choice, const std::optional<std::string>& model_path,
                                                   std::string& note, std::string& error) {
    if (choice == ParserChoice::rules) return std::make_unique<intent::RuleParser>();
    const auto loc = locate_model(default_model_search(model_path));
    if (loc.source == ModelSource::none) {
        note = "no model file found; using rules (searched:";
        for (const auto& c : loc.searched) note += " " + c;
        note += ")";
        return std::make_unique<intent::RuleParser>();
    }
    auto model = IntentModel::load(loc.path);
    if (!model) {
        const std::string why =
            "model " + loc.path + " (" + std::string(to_string(loc.source)) + ") failed to load: " + model.error().describe();
        if (loc.user_requested()) {
            error = why;
            return nullptr;
        }
        note = why + "; using rules";
        return std::make_unique<intent::RuleParser>();
    }
    note = "model: " + loc.path;
    auto shared = std::make_shared<IntentModel>(std::move(model).value());
    if (choice == ParserChoice::neural) return std::make_unique<NeuralParser>(shared);
    return std::make_unique<HybridParser>(shared);
}

}  // namespace osie::inference
