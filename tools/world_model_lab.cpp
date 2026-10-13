// SPDX-License-Identifier: Apache-2.0
// osie_wm_lab: data collection and feature encoding for the CPU world model
// (docs/WORLD_MODEL_EXPERIMENTS.md).
//
//   osie_wm_lab serve      (Linux only) open-loop request server plus
//                          telemetry. Controlled by line commands on stdin,
//                          replies are JSON lines on stdout:
//       CALIBRATE                          measure work units per ms
//       CONFIG family=F rate=R workers=N seed=S
//       RATE factor=X                      offered-load intervention
//       WINDOWS n=N ms=M tag=T             sample N windows of M ms
//       QUIT
//   osie_wm_lab encode     (portable) reads episodes as TSV on stdin and
//                          writes their feature vectors with the same encoder
//                          OSIE uses at prediction time.
//   osie_wm_lab predict M  (portable) like encode, but prints model M's predictions
//   osie_wm_lab names      prints the feature names
//
// `serve` must run inside a disposable, resource-limited container. It never
// changes its own limits: CPU quota and cpuset interventions are applied
// from outside by the experiment driver (ml/world_model/collect.py).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "osie/prediction/features.hpp"
#include "osie/prediction/world_model.hpp"
#include "osie/util/json.hpp"

#if defined(__linux__)
#include <time.h>
#include <unistd.h>

#include "osie/platform/os.hpp"
#endif

using namespace osie;
using namespace osie::predict;

namespace {

std::map<std::string, std::string> parse_kv(std::istringstream& is) {
    std::map<std::string, std::string> m;
    for (std::string tok; is >> tok;) {
        const auto eq = tok.find('=');
        if (eq != std::string::npos) m[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return m;
}

void write_window_fields(json::Writer& j, const CgroupWindow& w) {
    j.kv("secs", w.secs).kv("usage_cores", w.usage_cores).kv("throttled_ratio", w.throttled_ratio)
        .kv("throttled_frac", w.throttled_frac).kv("psi_some", w.psi_some).kv("psi_full", w.psi_full)
        .kv("quota_cores", w.quota_cores).kv("cpuset_cpus", w.cpuset_cpus).kv("pids", w.pids)
        .kv("host_busy_frac", w.host_busy_frac).kv("host_cpus", w.host_cpus);
}

// ------------------------------------------------------------------ encode ---
// TSV per episode: id <TAB> action previous proposed <TAB> window;window;...
// where each window is 11 comma-separated values in CgroupWindow field order
// (secs, usage, throttled_ratio, throttled_frac, psi_some, psi_full, quota,
// cpuset, pids, host_busy, host_cpus) and "nan" marks a missing value.
struct TsvEpisode {
    std::string id;
    Intervention a;
    std::vector<CgroupWindow> hist;
};

std::optional<TsvEpisode> parse_tsv(const std::string& line) {
    auto opt = [](double v) { return std::isnan(v) ? std::optional<double>{} : std::optional<double>{v}; };
    std::istringstream ls(line);
    TsvEpisode ep;
    std::string act, wins;
    if (!std::getline(ls, ep.id, '\t') || !std::getline(ls, act, '\t') || !std::getline(ls, wins, '\t')) return std::nullopt;
    std::istringstream as(act);
    std::string kind;
    as >> kind >> ep.a.previous >> ep.a.proposed;
    auto k = action_from_string(kind);
    if (!k) return std::nullopt;
    ep.a.kind = *k;
    std::istringstream ws(wins);
    for (std::string one; std::getline(ws, one, ';');) {
        std::vector<double> v;
        std::istringstream vs(one);
        for (std::string x; std::getline(vs, x, ',');) v.push_back(x == "nan" ? std::nan("") : std::stod(x));
        if (v.size() != 11) return std::nullopt;
        CgroupWindow w;
        w.secs = v[0];
        w.usage_cores = v[1];
        w.throttled_ratio = opt(v[2]);
        w.throttled_frac = opt(v[3]);
        w.psi_some = opt(v[4]);
        w.psi_full = opt(v[5]);
        w.quota_cores = opt(v[6]);
        w.cpuset_cpus = opt(v[7]);
        w.pids = opt(v[8]);
        w.host_busy_frac = opt(v[9]);
        w.host_cpus = opt(v[10]);
        ep.hist.push_back(w);
    }
    return ep;
}

/// predict: like encode, but prints the model's (bounded) predictions:
///   id OK|ABSTAIN|ERROR  expected values in target order
int run_predict(const std::string& model_path) {
    auto m = WorldModel::load(model_path);
    if (!m) {
        std::fprintf(stderr, "%s\n", m.error().describe().c_str());
        return 1;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        auto ep = parse_tsv(line);
        if (!ep) continue;
        auto p = (*m)->predict(ep->hist, ep->a);
        if (!p) {
            std::printf("%s\tERROR\t%s\n", ep->id.c_str(), p.error().message.c_str());
            continue;
        }
        std::printf("%s\t%s\t", ep->id.c_str(), p->abstained ? "ABSTAIN" : "OK");
        for (std::size_t i = 0; i < p->targets.size(); ++i) std::printf(i ? ",%.9g" : "%.9g", p->targets[i].expected);
        std::printf("\n");
    }
    return 0;
}

/// predict-json M [ARCH KERNEL]: one JSON object per episode with everything the
/// deployed predictor returns (intervals, abstention and reasons, throttling
/// assessment, clamping). With ARCH and KERNEL the environment check is applied.
int run_predict_json(const std::string& model_path, const std::string& arch, const std::string& kernel) {
    auto m = WorldModel::load(model_path);
    if (!m) {
        std::fprintf(stderr, "%s\n", m.error().describe().c_str());
        return 1;
    }
    for (std::string line; std::getline(std::cin, line);) {
        auto ep = parse_tsv(line);
        if (!ep) continue;
        auto p = arch.empty() ? (*m)->predict(ep->hist, ep->a)
                              : (*m)->predict_in(ep->hist, ep->a, RuntimeEnvironment{arch, kernel, 2});
        json::Writer w;
        w.begin_object().kv("id", ep->id);
        if (!p) {
            w.kv("error", p.error().message).end_object();
            std::puts(w.str().c_str());
            continue;
        }
        w.kv("abstained", p->abstained).kv("ood", p->out_of_distribution).kv("assessment", p->throttling_assessment);
        w.key("reasons").begin_array();
        for (const auto& r : p->reasons) w.value(r);
        w.end_array().key("targets").begin_object();
        for (const auto& t : p->targets) {
            w.key(t.name).begin_object().kv("expected", t.expected).kv("clamped", t.clamped)
                .kv("residual_applied", t.residual_applied).kv("status", t.status).key("intervals").begin_array();
            for (const auto& iv : t.intervals) w.begin_array().value(iv.level).value(iv.lo).value(iv.hi).end_array();
            w.end_array().end_object();
        }
        w.end_object().end_object();
        std::puts(w.str().c_str());
    }
    return 0;
}

/// bench: loads model M and times predict() on the episodes given on stdin
/// (feature encoding + model + bounds + intervals), single thread.
int run_bench(const std::string& model_path) {
    const auto t0 = std::chrono::steady_clock::now();
    auto m = WorldModel::load(model_path);
    const double load_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (!m) {
        std::fprintf(stderr, "%s\n", m.error().describe().c_str());
        return 1;
    }
    std::vector<TsvEpisode> eps;
    for (std::string line; std::getline(std::cin, line);)
        if (auto ep = parse_tsv(line)) eps.push_back(*ep);
    if (eps.empty()) return 1;
    std::vector<double> us;
    for (int rep = 0; rep < 200; ++rep)
        for (const auto& ep : eps) {
            const auto a = std::chrono::steady_clock::now();
            auto p = (*m)->predict(ep.hist, ep.a);
            const auto b = std::chrono::steady_clock::now();
            if (p) us.push_back(std::chrono::duration<double, std::micro>(b - a).count());
        }
    std::sort(us.begin(), us.end());
    auto q = [&](double f) { return us[std::min(us.size() - 1, static_cast<std::size_t>(f * static_cast<double>(us.size())))]; };
    std::printf("{\"model\":\"%s\",\"params\":%zu,\"load_us\":%.1f,\"predictions\":%zu,\"p50_us\":%.2f,\"p95_us\":%.2f,\"p99_us\":%.2f}\n",
                (*m)->id().c_str(), (*m)->parameter_count(), load_us, us.size(), q(0.5), q(0.95), q(0.99));
    return 0;
}

int run_encode() {
    std::string line;
    std::size_t bad = 0;
    while (std::getline(std::cin, line)) {
        auto ep = parse_tsv(line);
        if (!ep) {
            ++bad;
            continue;
        }
        const auto& id = ep->id;
        const auto& hist = ep->hist;
        const auto& a = ep->a;
        auto e = encode(hist, a);
        if (!e) {
            std::printf("%s\tERROR\t%s\n", id.c_str(), e.error().message.c_str());
            continue;
        }
        std::printf("%s\tOK\t", id.c_str());
        for (std::size_t i = 0; i < e->values.size(); ++i) std::printf(i ? ",%.9g" : "%.9g", e->values[i]);
        std::printf("\n");
    }
    if (bad) std::fprintf(stderr, "%zu malformed lines skipped\n", bad);
    return 0;
}

#if defined(__linux__)
// ------------------------------------------------------------------- serve ---

using clk = std::chrono::steady_clock;
volatile std::uint64_t g_sink = 0;

std::uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t thread_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

void arith(std::uint64_t iters) {
    std::uint64_t x = 88172645463325252ULL;
    for (std::uint64_t i = 0; i < iters; ++i) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
    }
    g_sink = g_sink + x;
}

void hash(std::vector<std::uint32_t>& buf, std::size_t words) {
    std::uint32_t h = 2166136261U;
    words = std::min(words, buf.size());
    for (std::size_t i = 0; i < words; ++i) {
        h = (h ^ buf[i]) * 16777619U;
        buf[i] = h;
    }
    g_sink = g_sink + h;
}

/// Workload families. Each request costs a fixed amount of work, expressed in
/// milliseconds of single-thread CPU at calibration time.
struct Family {
    const char* name;
    const char* kind;  // arith | hash | hashbig (16 MiB working set per worker)
    double work_ms;
    double sleep_ms;   // blocking part after the work (I/O-like)
    bool bursty;       // on/off arrivals with the same mean rate
    double work_sigma{0};  // lognormal spread of per-request work (0 = fixed), mean preserved
    bool closed{false};    // closed loop: each worker issues its next request after a think time
};
// v0.1 families (development data) and v0.2 families that were added only
// after the v0.2 model was frozen (confirmatory test): variable, closed, wide, hashbig.
const Family kFamilies[] = {
    {"arith", "arith", 2.0, 0.0, false},   {"hash", "hash", 2.0, 0.0, false},
    {"short", "arith", 0.3, 0.0, false},   {"burst", "arith", 1.0, 0.0, true},
    {"mixed", "arith", 1.0, 2.0, false},   {"variable", "arith", 1.5, 0.0, false, 1.0, false},
    {"closed", "arith", 2.0, 0.0, false, 0.0, true}, {"wide", "arith", 1.0, 0.0, false},
    {"hashbig", "hashbig", 2.0, 0.0, false},
};

class Server {
public:
    double arith_per_ms{0}, hash_words_per_ms{0};

    void calibrate() {
        auto measure = [](auto fn) {
            std::uint64_t n = 0;
            const auto t0 = thread_cpu_ns();
            while (thread_cpu_ns() - t0 < 300'000'000ULL) {
                fn();
                ++n;
            }
            return static_cast<double>(n) / (static_cast<double>(thread_cpu_ns() - t0) / 1e6);
        };
        arith_per_ms = 10000.0 * measure([] { arith(10000); });
        std::vector<std::uint32_t> buf(1 << 16, 1);
        hash_words_per_ms = static_cast<double>(buf.size()) * measure([&] { hash(buf, buf.size()); });
    }

    bool configure(const std::string& family, double rate, int workers, unsigned seed) {
        stop();
        fam_ = nullptr;
        for (const auto& f : kFamilies)
            if (family == f.name) fam_ = &f;
        if (fam_ == nullptr || rate <= 0 || workers < 1 || workers > 32) return false;
        rate_.store(rate);
        running_ = true;
        {
            std::lock_guard lk(mu_);
            queue_.clear();
            reset_stats_locked();
        }
        workers_ = workers;
        seed_ = seed;
        if (!fam_->closed) gen_ = std::thread([this, seed] { generate(seed); });
        for (int i = 0; i < workers; ++i) pool_.emplace_back([this, i] { work(i); });
        return true;
    }

    void scale_rate(double f) { rate_.store(rate_.load() * f); }
    double rate() const { return rate_.load(); }

    struct Stats {
        std::vector<double> lat_ms;
        double cpu_ms_sum{0};
        std::uint64_t arrivals{0}, dropped{0};
        std::size_t queue{0};
    };
    Stats take_stats() {
        std::lock_guard lk(mu_);
        Stats s;
        s.lat_ms.swap(lat_ms_);
        s.cpu_ms_sum = cpu_ms_;
        s.arrivals = arrivals_;
        s.dropped = dropped_;
        s.queue = queue_.size();
        reset_stats_locked();
        return s;
    }

    void stop() {
        running_ = false;
        cv_.notify_all();
        if (gen_.joinable()) gen_.join();
        for (auto& t : pool_) t.join();
        pool_.clear();
    }
    ~Server() { stop(); }

private:
    void reset_stats_locked() {
        lat_ms_.clear();
        cpu_ms_ = 0;
        arrivals_ = dropped_ = 0;
    }

    void generate(unsigned seed) {
        std::mt19937_64 rng(seed);
        std::exponential_distribution<double> expo(1.0);
        std::uint64_t t = now_ns();
        const std::uint64_t start = t;
        while (running_) {
            double r = rate_.load();
            if (fam_->bursty) {  // 100 ms on at 3x the rate, 200 ms off: same mean
                const auto phase = ((t - start) / 1'000'000ULL) % 300;
                if (phase >= 100) {
                    t = start + ((t - start) / 300'000'000ULL + 1) * 300'000'000ULL;
                    continue;
                }
                r *= 3.0;
            }
            t += static_cast<std::uint64_t>(expo(rng) / r * 1e9);
            const std::uint64_t n = now_ns();
            if (t > n + 200'000) {
                timespec ts{static_cast<time_t>(t / 1'000'000'000ULL), static_cast<long>(t % 1'000'000'000ULL)};
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            }
            {
                std::lock_guard lk(mu_);
                ++arrivals_;
                if (queue_.size() >= kMaxQueue) {
                    ++dropped_;
                    continue;
                }
                queue_.push_back(t);
            }
            cv_.notify_one();
        }
    }

    void work(int idx) {
        const bool big = std::strcmp(fam_->kind, "hashbig") == 0;
        std::vector<std::uint32_t> buf(big ? (4U << 20) : (1U << 20), static_cast<std::uint32_t>(idx));
        std::mt19937_64 rng(seed_ * 7919ULL + static_cast<unsigned>(idx));
        std::exponential_distribution<double> expo(1.0);
        const double sigma = fam_->work_sigma;
        std::lognormal_distribution<double> spread(-0.5 * sigma * sigma, sigma);  // mean 1
        std::size_t cursor = 0;
        while (true) {
            std::uint64_t arrival = 0;
            if (fam_->closed) {
                // Think time so that all workers together offer `rate` requests/s.
                const double per_worker = std::max(1e-3, rate_.load() / std::max(1, workers_));
                const double think_s = std::max(0.0, 1.0 / per_worker - fam_->work_ms / 1000.0) * expo(rng);
                const auto until = now_ns() + static_cast<std::uint64_t>(think_s * 1e9);
                while (running_ && now_ns() < until)
                    std::this_thread::sleep_for(std::chrono::microseconds(
                        std::min<std::uint64_t>(20'000, (until - std::min(until, now_ns())) / 1000 + 1)));
                if (!running_) return;
                arrival = now_ns();
                std::lock_guard lk(mu_);
                ++arrivals_;
            } else {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [this] { return !queue_.empty() || !running_; });
                if (!running_) return;
                arrival = queue_.front();
                queue_.pop_front();
            }
            const double scale = sigma > 0 ? std::min(20.0, spread(rng)) : 1.0;
            const auto c0 = thread_cpu_ns();
            if (std::strcmp(fam_->kind, "hash") == 0) {
                hash(buf, static_cast<std::size_t>(fam_->work_ms * scale * hash_words_per_ms));
            } else if (big) {
                // Same work as `hash`, striding through a 16 MiB buffer so the
                // working set does not fit in cache (memory-bound requests).
                const auto words = static_cast<std::size_t>(fam_->work_ms * scale * hash_words_per_ms);
                std::uint32_t h = 2166136261U;
                for (std::size_t i = 0; i < words; ++i) {
                    cursor = (cursor + 4099) & (buf.size() - 1);
                    h = (h ^ buf[cursor]) * 16777619U;
                    buf[cursor] = h;
                }
                g_sink = g_sink + h;
            } else {
                arith(static_cast<std::uint64_t>(fam_->work_ms * scale * arith_per_ms));
            }
            const auto c1 = thread_cpu_ns();
            if (fam_->sleep_ms > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(fam_->sleep_ms * 1000)));
            const double lat = static_cast<double>(now_ns() - std::min(arrival, now_ns())) / 1e6;
            std::lock_guard lk(mu_);
            lat_ms_.push_back(lat);
            cpu_ms_ += static_cast<double>(c1 - c0) / 1e6;
        }
    }

    static constexpr std::size_t kMaxQueue = 20000;
    const Family* fam_{nullptr};
    int workers_{1};
    unsigned seed_{0};
    std::atomic<double> rate_{0};
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::uint64_t> queue_;
    std::vector<double> lat_ms_;
    double cpu_ms_{0};
    std::uint64_t arrivals_{0}, dropped_{0};
    std::thread gen_;
    std::vector<std::thread> pool_;
};

struct Snapshot {
    std::optional<CgroupInfo> cg;
    std::optional<CpuSample> host;
    std::uint64_t t_ns{0};
    double cost_us{0};  // time spent reading telemetry (observer overhead)
};

int run_serve() {
    auto os = make_native_os();
    auto* lx = os->linux_ext();
    if (lx == nullptr) {
        std::puts(R"({"type":"error","message":"not linux"})");
        return 1;
    }
    std::string cg_path = "/";
    if (auto r = lx->cgroup_of(::getpid())) cg_path = *r;
    auto snap = [&]() {
        Snapshot s;
        const auto c0 = now_ns();
        if (auto c = lx->cgroup(cg_path)) s.cg = c->value;
        if (auto h = os->cpu_sample()) s.host = h->value;
        s.t_ns = now_ns();
        s.cost_us = static_cast<double>(s.t_ns - c0) / 1000.0;
        return s;
    };
    Server srv;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        auto kv = parse_kv(is);
        json::Writer j;
        j.begin_object();
        if (cmd == "CALIBRATE") {
            srv.calibrate();
            j.kv("type", "calib").kv("arith_iters_per_ms", srv.arith_per_ms).kv("hash_words_per_ms", srv.hash_words_per_ms)
                .kv("cgroup", cg_path);
        } else if (cmd == "CONFIG") {
            const bool ok = srv.configure(kv["family"], std::stod(kv.count("rate") ? kv["rate"] : "0"),
                                          std::stoi(kv.count("workers") ? kv["workers"] : "0"),
                                          static_cast<unsigned>(std::stoul(kv.count("seed") ? kv["seed"] : "1")));
            j.kv("type", ok ? "ok" : "error").kv("rate", srv.rate());
        } else if (cmd == "RATE") {
            srv.scale_rate(std::stod(kv["factor"]));
            j.kv("type", "ok").kv("rate", srv.rate());
        } else if (cmd == "WINDOWS") {
            const int n = std::stoi(kv["n"]);
            const long ms = std::stol(kv["ms"]);
            j.kv("type", "windows").kv("tag", kv["tag"]).kv("offered_rate", srv.rate()).key("windows").begin_array();
            Snapshot prev = snap();
            (void)srv.take_stats();
            for (int i = 0; i < n; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                Snapshot cur = snap();
                auto st = srv.take_stats();
                const double secs = static_cast<double>(cur.t_ns - prev.t_ns) / 1e9;
                j.begin_object();
                if (prev.cg && cur.cg) {
                    if (auto w = make_window(*prev.cg, *cur.cg, secs, prev.host ? &*prev.host : nullptr,
                                             cur.host ? &*cur.host : nullptr))
                        write_window_fields(j, *w);
                }
                std::sort(st.lat_ms.begin(), st.lat_ms.end());
                auto q = [&](double p) -> std::optional<double> {
                    if (st.lat_ms.empty()) return std::nullopt;
                    return st.lat_ms[std::min(st.lat_ms.size() - 1, static_cast<std::size_t>(p * static_cast<double>(st.lat_ms.size())))];
                };
                const double done = static_cast<double>(st.lat_ms.size());
                j.kv("arrivals_per_s", static_cast<double>(st.arrivals) / secs).kv("completed_per_s", done / secs)
                    .kv("dropped", st.dropped).kv("queue_end", static_cast<std::uint64_t>(st.queue))
                    .kv("lat_p50_ms", q(0.5)).kv("lat_p95_ms", q(0.95)).kv("lat_p99_ms", q(0.99))
                    .kv("cpu_ms_per_req", done > 0 ? std::optional<double>(st.cpu_ms_sum / done) : std::nullopt)
                    .kv("observer_cost_us", cur.cost_us);
                if (cur.host) {
                    j.kv("runnable", cur.host->runnable_tasks).kv("load1", cur.host->load ? std::optional<double>(cur.host->load->one) : std::nullopt);
                    if (prev.host && cur.host->context_switches_total && prev.host->context_switches_total)
                        j.kv("ctxt_per_s", static_cast<double>(*cur.host->context_switches_total - *prev.host->context_switches_total) / secs);
                }
                j.end_object();
                prev = cur;
            }
            j.end_array();
        } else if (cmd == "QUIT") {
            srv.stop();
            break;
        } else {
            j.kv("type", "error").kv("message", "unknown command");
        }
        j.end_object();
        std::puts(j.str().c_str());
        std::fflush(stdout);
    }
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "encode") return run_encode();
    if (mode == "predict" && argc > 2) return run_predict(argv[2]);
    if (mode == "bench" && argc > 2) return run_bench(argv[2]);
    if (mode == "predict-json" && argc > 2)
        return run_predict_json(argv[2], argc > 4 ? argv[3] : "", argc > 4 ? argv[4] : "");
    if (mode == "names") {
        for (const auto& n : feature_names()) std::puts(n.c_str());
        return 0;
    }
#if defined(__linux__)
    if (mode == "serve") return run_serve();
#endif
    std::fprintf(stderr, "usage: osie_wm_lab serve|encode|names\n");
    return 2;
}
