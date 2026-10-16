// SPDX-License-Identifier: Apache-2.0
#include "osie/agent/config.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <set>
#include <sstream>

#include "osie/util/secure_path.hpp"

namespace osie::agent {
namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

Error bad(std::size_t line, const std::string& m) {
    return Error{Errc::invalid_argument, "agent config line " + std::to_string(line) + ": " + m, 0};
}

std::optional<long long> parse_int(const std::string& v) {
    if (v.empty() || v.size() > 12) return std::nullopt;
    long long r = 0;
    for (char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        r = r * 10 + (c - '0');
    }
    return r;
}

std::optional<Millis> parse_duration(const std::string& v) {
    if (v == "0") return Millis{0};  // "never" / "automatic"; ranges still apply
    std::size_t i = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
    const auto n = parse_int(v.substr(0, i));
    if (!n) return std::nullopt;
    const std::string unit = v.substr(i);
    long long ms = 0;
    if (unit == "ms") ms = *n;
    else if (unit == "s") ms = *n * 1000;
    else if (unit == "m") ms = *n * 60'000;
    else if (unit == "h") ms = *n * 3'600'000;
    else return std::nullopt;
    return Millis{ms};
}

std::optional<ObservationKind> kind_from(std::string_view s) {
    for (unsigned k = 0; k < static_cast<unsigned>(ObservationKind::count_); ++k)
        if (to_string(static_cast<ObservationKind>(k)) == s) return static_cast<ObservationKind>(k);
    return std::nullopt;
}

/// A socket path the agent may create: absolute, short enough for
/// sockaddr_un on every platform, plain characters, no "." or ".." parts.
bool valid_socket_path(const std::string& p) {
    if (p.size() < 2 || p.size() > 100 || p[0] != '/' || p.back() == '/') return false;
    for (char c : p)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '_' || c == '-')) return false;
    std::size_t i = 1;
    while (i <= p.size()) {
        const std::size_t j = std::min(p.find('/', i), p.size());
        const std::string part = p.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        i = j + 1;
    }
    return true;
}

bool valid_group_name(const std::string& g) {
    if (g.empty() || g.size() > 32) return false;
    if (!(std::islower(static_cast<unsigned char>(g[0])) || g[0] == '_')) return false;
    for (char c : g)
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            return false;
    return true;
}

std::string dur(Millis m) {
    const auto v = m.count();
    if (v % 3'600'000 == 0 && v) return std::to_string(v / 3'600'000) + "h";
    if (v % 60'000 == 0 && v) return std::to_string(v / 60'000) + "m";
    if (v % 1000 == 0 && v) return std::to_string(v / 1000) + "s";
    return std::to_string(v) + "ms";
}

}  // namespace

std::string_view to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::error: return "error";
        case LogLevel::warn: return "warn";
        case LogLevel::info: return "info";
        case LogLevel::debug: return "debug";
    }
    return "?";
}

Result<AgentConfig> parse_agent_config(std::string_view text) {
    if (text.size() > 64 * 1024) return Error{Errc::invalid_argument, "agent config larger than 64 KiB", 0};
    AgentConfig c;
    std::set<std::string> seen;
    std::istringstream in{std::string(text)};
    std::string raw;
    std::size_t lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        if (lineno > 1000) return bad(lineno, "too many lines");
        const auto hash = raw.find('#');
        const std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) return bad(lineno, "expected 'key = value'");
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) return bad(lineno, "empty key or value");
        if (!seen.insert(key).second) return bad(lineno, "duplicate key '" + key + "'");
        auto duration_in = [&](Millis lo, Millis hi, Millis& out) -> Result<void> {
            const auto d = parse_duration(val);
            if (!d) return bad(lineno, key + ": expected a duration such as 500ms, 5s, 2m or 1h");
            if (*d < lo || *d > hi) return bad(lineno, key + " must be between " + dur(lo) + " and " + dur(hi));
            out = *d;
            return {};
        };
        auto count_in = [&](long long lo, long long hi, std::size_t& out) -> Result<void> {
            const auto n = parse_int(val);
            if (!n) return bad(lineno, key + ": expected a non-negative integer");
            if (*n < lo || *n > hi) return bad(lineno, key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
            out = static_cast<std::size_t>(*n);
            return {};
        };
        Result<void> r;
        if (key == "interval") r = duration_in(Millis{250}, Millis{3'600'000}, c.interval);
        else if (key == "sample_window") r = duration_in(Millis{50}, Millis{5000}, c.sample_window);
        else if (key == "history_capacity") r = count_in(8, 100'000, c.history_capacity);
        else if (key == "history_top_k") r = count_in(1, 64, c.history_top_k);
        else if (key == "cgroup_top_n") r = count_in(0, 64, c.cgroup_top_n);
        else if (key == "failure_threshold") r = count_in(1, 100, c.failure_threshold);
        else if (key == "health_log_interval") r = duration_in(Millis{0}, Millis{86'400'000}, c.health_log_interval);
        else if (key == "api_enabled") {
            if (val == "true") c.api_enabled = true;
            else if (val == "false") c.api_enabled = false;
            else r = bad(lineno, "api_enabled must be true or false");
        } else if (key == "api_socket") {
            if (!valid_socket_path(val))
                r = bad(lineno, "api_socket must be an absolute path of at most 100 characters (letters, digits, '/', "
                                "'.', '_', '-'; no '.' or '..' components)");
            else c.api_socket = val;
        } else if (key == "api_group") {
            if (!valid_group_name(val)) r = bad(lineno, "api_group must be a group name (lowercase letters, digits, '_', '-')");
            else c.api_group = val;
        }
        else if (key == "api_max_clients") r = count_in(1, 64, c.api_max_clients);
        else if (key == "api_max_request_bytes") r = count_in(1024, 1024 * 1024, c.api_max_request_bytes);
        else if (key == "api_request_timeout") r = duration_in(Millis{100}, Millis{60'000}, c.api_request_timeout);
        else if (key == "api_max_sessions") r = count_in(1, 4096, c.api_max_sessions);
        else if (key == "api_session_ttl") r = duration_in(Millis{10'000}, Millis{86'400'000}, c.api_session_ttl);
        else if (key == "api_max_evidence_age") r = duration_in(Millis{0}, Millis{3'600'000}, c.api_max_evidence_age);
        else if (key == "api_rate_limit") r = count_in(1, 10'000, c.api_rate_limit);
        else if (key == "log_level") {
            if (val == "error") c.log_level = LogLevel::error;
            else if (val == "warn") c.log_level = LogLevel::warn;
            else if (val == "info") c.log_level = LogLevel::info;
            else if (val == "debug") c.log_level = LogLevel::debug;
            else r = bad(lineno, "log_level must be error, warn, info or debug");
        } else if (key == "observe") {
            c.observe.clear();
            std::istringstream ks(val);
            for (std::string item; std::getline(ks, item, ',');) {
                const auto k = kind_from(trim(item));
                if (!k) return bad(lineno, "unknown observation kind '" + trim(item) + "'");
                if (std::find(c.observe.begin(), c.observe.end(), *k) == c.observe.end()) c.observe.push_back(*k);
            }
            if (c.observe.empty()) r = bad(lineno, "observe must name at least one observation kind");
        } else if (key == "listen" || key == "bind" || key == "port" || key == "persist" || key == "persistence" ||
                   key == "database" || key == "remote" || key == "upload" || key == "endpoint") {
            r = bad(lineno, "'" + key + "' is not supported: this agent keeps all data in memory and has no network "
                            "listener or remote access (its local API is a Unix domain socket: api_enabled, api_socket)");
        } else {
            r = bad(lineno, "unknown key '" + key + "'");
        }
        if (!r) return r.error();
    }
    // Cross-field rules.
    if (c.sample_window * 2 > c.interval)
        return Error{Errc::invalid_argument,
                     "agent config: sample_window (" + dur(c.sample_window) + ") must be at most half of interval (" +
                         dur(c.interval) + ")",
                     0};
    return c;
}

namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;

}  // namespace

Result<AgentConfig> load_agent_config(const std::string& path) {
    auto f = secure::open_trusted_file(path, secure::OpenOptions{"agent config", false});
    if (!f) return f.error();
    // The size limit is enforced on the bytes read (st_size can change, or lie).
    std::string text;
    char buf[8192];
    while (true) {
        const auto n = ::read(f->get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Error{Errc::io_error, "agent config " + path + ": " + std::strerror(errno), errno};
        }
        if (n == 0) break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() > kMaxConfigBytes) return Error{Errc::invalid_argument, "agent config " + path + " is larger than 64 KiB", 0};
    }
    return parse_agent_config(text);
}

std::string to_text(const AgentConfig& c) {
    std::string o;
    o += "interval = " + dur(c.interval) + "\n";
    o += "sample_window = " + dur(c.sample_window) + "\n";
    o += "history_capacity = " + std::to_string(c.history_capacity) + "\n";
    o += "history_top_k = " + std::to_string(c.history_top_k) + "\n";
    o += "cgroup_top_n = " + std::to_string(c.cgroup_top_n) + "\n";
    o += "failure_threshold = " + std::to_string(c.failure_threshold) + "\n";
    o += "health_log_interval = " + dur(c.health_log_interval) + "\n";
    o += "log_level = " + std::string(to_string(c.log_level)) + "\n";
    o += "observe = ";
    for (std::size_t i = 0; i < c.observe.size(); ++i) o += (i ? "," : "") + std::string(to_string(c.observe[i]));
    o += "\n";
    o += "api_enabled = " + std::string(c.api_enabled ? "true" : "false") + "\n";
    o += "api_socket = " + c.api_socket + "\n";
    if (!c.api_group.empty()) o += "api_group = " + c.api_group + "\n";
    o += "api_max_clients = " + std::to_string(c.api_max_clients) + "\n";
    o += "api_max_request_bytes = " + std::to_string(c.api_max_request_bytes) + "\n";
    o += "api_request_timeout = " + dur(c.api_request_timeout) + "\n";
    o += "api_max_sessions = " + std::to_string(c.api_max_sessions) + "\n";
    o += "api_session_ttl = " + dur(c.api_session_ttl) + "\n";
    o += "api_max_evidence_age = " + dur(c.api_max_evidence_age) + "\n";
    o += "api_rate_limit = " + std::to_string(c.api_rate_limit) + "\n";
    return o;
}

}  // namespace osie::agent
