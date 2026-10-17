// SPDX-License-Identifier: Apache-2.0
#include "osie/remote/cli.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "osie/remote/errors.hpp"
#include "osie/remote/http.hpp"
#include "osie/remote/profile.hpp"
#include "osie/remote/ssh.hpp"
#include "osie/remote/transport.hpp"
#include "osie/util/json.hpp"

namespace osie::remote {
namespace {

constexpr int kExitAnswer = 0, kExitApiError = 1, kExitUsage = 2, kExitTransport = 3;

void usage(std::FILE* to) {
    std::fputs(
        "usage: osie remote <command> [options]\n"
        "Ask an osie-agent on another host, over OpenSSH (docs/REMOTE.md).\n"
        "commands:\n"
        "  add NAME [USER@]HOST [--port N] [--socket PATH] [--identity PATH]\n"
        "                       [--known-hosts PATH] [--timeout S]\n"
        "                                 save a profile (no secrets are stored)\n"
        "  list [--json]                  list profiles\n"
        "  remove NAME                    delete a profile\n"
        "  status NAME [--json]           readiness and access tier, with latency\n"
        "  ask NAME \"question\" [--session ID] [--max-evidence-age-ms N] [--json]\n"
        "  snapshot NAME [--json]         the agent's newest snapshot\n"
        "  history NAME [--window-seconds N] [--limit N] [--metric M] [--json]\n"
        "  capabilities NAME [--json]     what the remote agent observes and answers\n"
        "options for status, ask, snapshot, history, capabilities:\n"
        "  --timeout S          overall deadline, 1-120 seconds (default: the profile's, else 15)\n"
        "  --json               the agent's response body, byte for byte\n"
        "exit status: 0 answered, 1 the agent returned an error, 2 usage, 3 transport failure\n",
        to);
}

int usage_error(const std::string& msg) {
    std::fprintf(stderr, "osie remote: %s\n", msg.c_str());
    std::fputs("run 'osie remote help' for usage\n", stderr);
    return kExitUsage;
}

/// Parsed command line of one subcommand.
struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;  // --flag -> value
    std::set<std::string> flags;             // boolean flags
};

/// Strict: only the given value options and boolean flags; "--" ends options.
std::optional<std::string> parse(int argc, char** argv, int from, const std::set<std::string>& value_opts,
                                 const std::set<std::string>& bool_opts, Args& a) {
    bool only_pos = false;
    for (int i = from; i < argc; ++i) {
        const std::string s = argv[i];
        if (only_pos || s.empty() || s[0] != '-' || s == "-") {
            a.pos.push_back(s);
            continue;
        }
        if (s == "--") {
            only_pos = true;
            continue;
        }
        if (bool_opts.count(s) != 0) {
            a.flags.insert(s);
            continue;
        }
        if (value_opts.count(s) != 0) {
            if (i + 1 >= argc) return s + " needs a value";
            if (a.opt.count(s) != 0) return s + " given twice";
            a.opt[s] = argv[++i];
            continue;
        }
        return "unknown option " + s;
    }
    return std::nullopt;
}

void write_all(std::FILE* f, std::string_view s) {
    if (!s.empty()) std::fwrite(s.data(), 1, s.size(), f);
    std::fflush(f);
}

std::string ensure_nl(std::string s) {
    if (s.empty() || s.back() != '\n') s += '\n';
    return s;
}

/// Loads the named profile. On failure prints and returns the exit status.
std::optional<Profile> load_profile(const std::string& name, bool as_json, int& exit_code) {
    auto fail = [&](const std::string& msg) {
        RemoteError e;
        e.code = RemoteErrorCode::invalid_profile;
        e.message = msg;
        if (as_json) write_all(stdout, error_envelope(e, name) + "\n");
        std::fprintf(stderr, "osie remote: %s\n", msg.c_str());
        exit_code = kExitUsage;
        return std::nullopt;
    };
    if (auto e = check_name(name)) return fail("invalid remote name: " + *e);
    auto store = ProfileStore::for_user();
    if (!store) return fail(store.error().message);
    auto all = store->load();
    if (!all) return fail(all.error().message);
    for (const auto& p : *all)
        if (p.name == name) return p;
    return fail("no remote named '" + name + "' (see 'osie remote list')");
}

/// Reports a transport failure. Returns the exit status.
int report_transport(const RemoteError& e, const std::string& name, bool as_json) {
    if (as_json) write_all(stdout, error_envelope(e, name) + "\n");
    std::string msg = "osie remote: " + name + ": " + std::string(to_string(e.code)) + ": " + e.message + "\n";
    if (!e.ssh_stderr.empty()) {
        msg += "  ssh:";
        std::size_t lines = 0, i = 0;
        const std::string& s = e.ssh_stderr;
        while (i < s.size() && lines < 6) {
            const std::size_t j = std::min(s.find('\n', i), s.size());
            if (j > i) {
                msg += (lines == 0 ? " " : "\n       ") + s.substr(i, j - i);
                ++lines;
            }
            i = j + 1;
        }
        msg += "\n";
    }
    write_all(stderr, msg);
    return kExitTransport;
}

/// The effective deadline: --timeout, else the profile's.
std::optional<int> timeout_of(const Args& a, const Profile& p, std::string& err) {
    const auto it = a.opt.find("--timeout");
    if (it == a.opt.end()) return p.timeout_s;
    const auto v = parse_decimal(it->second);
    if (!v) {
        err = "--timeout must be a whole number of seconds";
        return std::nullopt;
    }
    if (auto e = check_timeout(*v)) {
        err = "--timeout: " + *e;
        return std::nullopt;
    }
    return static_cast<int>(*v);
}

struct Exchange {
    std::string target;
    std::string request;
};

struct Answered {
    HttpReply reply;
    double ms{0};
};

/// Opens one SSH connection and performs the exchanges in order. Prints a
/// transport failure itself; returns nothing in that case.
struct RunResult {
    std::vector<Answered> answers;
    double ssh_ready_ms{0}, total_ms{0};
    int exit_code{0};
    bool ok{false};
};

RunResult run_exchanges(const Profile& p, int timeout_s, const std::vector<Exchange>& xs, bool as_json) {
    RunResult rr;
    const auto t0 = Clock::now();
    SignalGuard guard;
    auto ssh = locate_ssh();
    if (!ssh) {
        RemoteError e;
        e.code = RemoteErrorCode::ssh_unavailable;
        e.message = ssh.error().message;
        rr.exit_code = report_transport(e, p.name, as_json);
        return rr;
    }
    TunnelOptions o;
    o.ssh_path = *ssh;
    o.timeout = std::chrono::seconds(timeout_s);
    o.cancel_fd = guard.fd();
    std::optional<RemoteError> failure;
    {
        auto t = SshTunnel::open(p, o);
        if (!t) {
            failure = t.error();
        } else {
            rr.ssh_ready_ms = t.value()->ready_ms();
            for (const auto& x : xs) {
                const auto s = Clock::now();
                auto r = t.value()->exchange(x.request);
                if (!r) {
                    failure = r.error();
                    break;
                }
                rr.answers.push_back(Answered{r.value(), std::chrono::duration<double, std::milli>(Clock::now() - s).count()});
            }
        }
        // The tunnel is closed here: ssh stopped and reaped, directory removed.
    }
    rr.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (guard.caught() != 0) {
        RemoteError e;
        e.code = RemoteErrorCode::interrupted;
        e.message = "Interrupted by a signal; ssh was stopped and the forwarding socket removed.";
        report_transport(e, p.name, as_json);
        guard.reraise();
        rr.exit_code = 128 + guard.caught();
        return rr;
    }
    if (failure) {
        rr.exit_code = report_transport(*failure, p.name, as_json);
        return rr;
    }
    rr.ok = true;
    return rr;
}

/// Prints one agent answer (ask, snapshot, history, capabilities).
int print_answer(const HttpReply& r, bool as_json, bool is_diagnose) {
    const bool ok = r.status >= 200 && r.status < 300;
    if (as_json) {
        write_all(stdout, r.body);  // byte for byte
    } else if (ok) {
        write_all(stdout, is_diagnose ? render_answer_text(r.body) : ensure_nl(pretty_json(r.body)));
    } else {
        write_all(stderr, render_error_text(r.body, r.status));
    }
    return ok ? kExitAnswer : kExitApiError;
}

std::string fmt_ms(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

// ------------------------------------------------------------ commands ---

int cmd_add(int argc, char** argv) {
    Args a;
    if (auto e = parse(argc, argv, 3, {"--port", "--socket", "--identity", "--known-hosts", "--timeout"}, {}, a))
        return usage_error(*e);
    if (a.pos.size() != 2) return usage_error("add needs NAME and [USER@]HOST");
    Profile p;
    p.name = a.pos[0];
    p.destination = a.pos[1];
    if (auto it = a.opt.find("--port"); it != a.opt.end()) {
        const auto v = parse_decimal(it->second);
        if (!v || check_port(*v)) return usage_error("--port must be from 1 to 65535");
        p.port = static_cast<int>(*v);
    }
    if (auto it = a.opt.find("--socket"); it != a.opt.end()) p.socket = it->second;
    if (auto it = a.opt.find("--identity"); it != a.opt.end()) p.identity = it->second;
    if (auto it = a.opt.find("--known-hosts"); it != a.opt.end()) p.known_hosts = it->second;
    if (auto it = a.opt.find("--timeout"); it != a.opt.end()) {
        const auto v = parse_decimal(it->second);
        if (!v) return usage_error("--timeout must be a whole number of seconds");
        if (auto e = check_timeout(*v)) return usage_error("--timeout: " + *e);
        p.timeout_s = static_cast<int>(*v);
    }
    if (auto e = check_name(p.name)) return usage_error("invalid name: " + *e);
    if (auto e = check_destination(p.destination)) return usage_error("invalid destination: " + *e);
    if (auto e = check_socket_path(p.socket)) return usage_error("--socket: " + *e);
    if (!p.identity.empty())
        if (auto e = check_file_path(p.identity)) return usage_error("--identity: " + *e);
    if (!p.known_hosts.empty())
        if (auto e = check_file_path(p.known_hosts)) return usage_error("--known-hosts: " + *e);
    auto store = ProfileStore::for_user();
    if (!store) return usage_error(store.error().message);
    auto r = store->update([&](std::vector<Profile>& ps) -> Result<void> {
        for (const auto& q : ps)
            if (q.name == p.name)
                return Error{Errc::invalid_argument, "a remote named '" + p.name + "' exists; remove it first", 0};
        if (ps.size() >= kMaxProfiles) return Error{Errc::invalid_argument, "too many remotes", 0};
        ps.push_back(p);
        return {};
    });
    if (!r) return usage_error(r.error().message);
    std::printf("added remote '%s' (%s)\n", p.name.c_str(), p.destination.c_str());
    return 0;
}

int cmd_remove(int argc, char** argv) {
    Args a;
    if (auto e = parse(argc, argv, 3, {}, {}, a)) return usage_error(*e);
    if (a.pos.size() != 1) return usage_error("remove needs NAME");
    const std::string name = a.pos[0];
    if (auto e = check_name(name)) return usage_error("invalid name: " + *e);
    auto store = ProfileStore::for_user();
    if (!store) return usage_error(store.error().message);
    auto r = store->update([&](std::vector<Profile>& ps) -> Result<void> {
        const auto n = ps.size();
        std::erase_if(ps, [&](const Profile& q) { return q.name == name; });
        if (ps.size() == n) return Error{Errc::not_found, "no remote named '" + name + "'", 0};
        return {};
    });
    if (!r) return usage_error(r.error().message);
    std::printf("removed remote '%s'\n", name.c_str());
    return 0;
}

int cmd_list(int argc, char** argv) {
    Args a;
    if (auto e = parse(argc, argv, 3, {}, {"--json"}, a)) return usage_error(*e);
    if (!a.pos.empty()) return usage_error("list takes no arguments");
    const bool as_json = a.flags.count("--json") != 0;
    auto store = ProfileStore::for_user();
    if (!store) return usage_error(store.error().message);
    auto all = store->load();
    if (!all) return usage_error(all.error().message);
    if (as_json) {
        json::Writer w;
        w.begin_object().kv("schema_version", "1.0").key("remotes").begin_array();
        for (const auto& p : *all) {
            w.begin_object().kv("name", p.name).kv("destination", p.destination);
            if (p.port) w.kv("port", *p.port);
            else w.key("port").null();
            w.kv("socket", p.socket);
            if (!p.identity.empty()) w.kv("identity", p.identity);
            if (!p.known_hosts.empty()) w.kv("known_hosts", p.known_hosts);
            w.kv("timeout_s", p.timeout_s).end_object();
        }
        w.end_array().end_object();
        write_all(stdout, w.str() + "\n");
        return 0;
    }
    if (all->empty()) {
        std::puts("no remotes (add one with 'osie remote add NAME USER@HOST')");
        return 0;
    }
    std::printf("%-20s %-32s %-6s %-26s %s\n", "NAME", "DESTINATION", "PORT", "SOCKET", "TIMEOUT");
    for (const auto& p : *all)
        std::printf("%-20s %-32s %-6s %-26s %ds\n", p.name.c_str(), p.destination.c_str(),
                    p.port ? std::to_string(*p.port).c_str() : "-", p.socket.c_str(), p.timeout_s);
    return 0;
}

/// ask, snapshot, history, capabilities: one request each.
int cmd_single(const std::string& sub, int argc, char** argv) {
    Args a;
    std::set<std::string> vals = {"--timeout"};
    if (sub == "ask") vals.insert({"--session", "--max-evidence-age-ms"});
    if (sub == "history") vals.insert({"--window-seconds", "--limit", "--metric"});
    if (auto e = parse(argc, argv, 3, vals, {"--json"}, a)) return usage_error(*e);
    if (a.pos.empty()) return usage_error(sub + " needs NAME");
    const bool as_json = a.flags.count("--json") != 0;
    Exchange x;
    if (sub == "ask") {
        if (a.pos.size() < 2) return usage_error("ask needs NAME and a question");
        DiagnoseRequest d;
        for (std::size_t i = 1; i < a.pos.size(); ++i) d.question += (i > 1 ? " " : "") + a.pos[i];
        if (d.question.empty()) return usage_error("the question is empty");
        if (auto it = a.opt.find("--session"); it != a.opt.end()) {
            if (it->second.empty()) return usage_error("--session needs a value");
            d.session_id = it->second;
        }
        if (auto it = a.opt.find("--max-evidence-age-ms"); it != a.opt.end()) {
            const auto v = parse_decimal(it->second);
            if (!v) return usage_error("--max-evidence-age-ms must be a whole number");
            d.max_evidence_age_ms = *v;
        }
        x.target = "/v1/diagnose";
        x.request = http_post_json(x.target, diagnose_body(d));
    } else {
        if (a.pos.size() != 1) return usage_error(sub + " takes only NAME");
        if (sub == "snapshot") x.target = "/v1/system/snapshot";
        else if (sub == "capabilities") x.target = "/v1/capabilities";
        else {
            auto opt = [&](const char* k) -> std::optional<std::string> {
                auto it = a.opt.find(k);
                return it == a.opt.end() ? std::nullopt : std::optional<std::string>(it->second);
            };
            auto t = history_target(opt("--window-seconds"), opt("--limit"), opt("--metric"));
            if (!t) return usage_error("--window-seconds and --limit take digits; --metric takes [a-z0-9_]");
            x.target = *t;
        }
        x.request = http_get(x.target);
    }
    int code = 0;
    const auto p = load_profile(a.pos[0], as_json, code);
    if (!p) return code;
    std::string terr;
    const auto timeout = timeout_of(a, *p, terr);
    if (!timeout) return usage_error(terr);
    auto rr = run_exchanges(*p, *timeout, {x}, as_json);
    if (!rr.ok) return rr.exit_code;
    return print_answer(rr.answers[0].reply, as_json, sub == "ask");
}

int cmd_status(int argc, char** argv) {
    Args a;
    if (auto e = parse(argc, argv, 3, {"--timeout"}, {"--json"}, a)) return usage_error(*e);
    if (a.pos.size() != 1) return usage_error("status needs NAME");
    const bool as_json = a.flags.count("--json") != 0;
    int code = 0;
    const auto p = load_profile(a.pos[0], as_json, code);
    if (!p) return code;
    std::string terr;
    const auto timeout = timeout_of(a, *p, terr);
    if (!timeout) return usage_error(terr);
    auto rr = run_exchanges(*p, *timeout,
                            {Exchange{"/v1/health/ready", http_get("/v1/health/ready")},
                             Exchange{"/v1/capabilities", http_get("/v1/capabilities")}},
                            as_json);
    if (!rr.ok) return rr.exit_code;
    const auto& ready = rr.answers[0];
    const auto& caps = rr.answers[1];
    const bool ready_ok = ready.reply.status >= 200 && ready.reply.status < 300;
    if (as_json) {
        // The agent's bodies are embedded verbatim (validated JSON, never re-serialised).
        auto part = [](const Answered& x) {
            return "{\"http_status\":" + std::to_string(x.reply.status) + ",\"latency_ms\":" + fmt_ms(x.ms) +
                   ",\"body\":" + x.reply.body + "}";
        };
        const std::string out = "{\"schema_version\":\"1.0\",\"remote\":\"" + json::escape(p->name) +
                                "\",\"ssh_connect_ms\":" + fmt_ms(rr.ssh_ready_ms) + ",\"total_ms\":" + fmt_ms(rr.total_ms) +
                                ",\"ready\":" + part(ready) + ",\"capabilities\":" + part(caps) + "}\n";
        write_all(stdout, out);
        return ready_ok ? kExitAnswer : kExitApiError;
    }
    const auto rdoc = parse_json(ready.reply.body);
    const std::string* rstatus = rdoc ? rdoc->str("status") : nullptr;
    std::string access;
    if (caps.reply.status >= 200 && caps.reply.status < 300) {
        access = "diagnostics (capabilities answered)";
    } else {
        const auto cdoc = parse_json(caps.reply.body);
        const auto* err = cdoc ? cdoc->get("error") : nullptr;
        const std::string* c = err ? err->str("code") : nullptr;
        access = (c != nullptr && *c == "forbidden") ? "health only (diagnostics not permitted for this user)"
                                                     : "capabilities unavailable (HTTP " + std::to_string(caps.reply.status) +
                                                           (c ? ", " + *c : std::string()) + ")";
    }
    std::printf("remote %s (%s)\n", p->name.c_str(), p->destination.c_str());
    std::printf("  agent:   %s (HTTP %d)\n", rstatus ? rstatus->c_str() : "unknown", ready.reply.status);
    std::printf("  access:  %s\n", access.c_str());
    std::printf("  latency: ssh connect %s ms, ready %s ms, capabilities %s ms, total %s ms\n", fmt_ms(rr.ssh_ready_ms).c_str(),
                fmt_ms(ready.ms).c_str(), fmt_ms(caps.ms).c_str(), fmt_ms(rr.total_ms).c_str());
    std::fflush(stdout);
    return ready_ok ? kExitAnswer : kExitApiError;
}

}  // namespace

std::string render_answer_text(std::string_view body) {
    const auto doc = parse_json(body);
    if (!doc) return "(unreadable answer)\n";
    std::string o;
    const auto* answer = doc->get("answer");
    const std::string* text = answer ? answer->str("text") : nullptr;
    o += ensure_nl(text ? *text : std::string("(no answer text)"));
    const std::string* status = doc->str("status");
    if (status != nullptr && *status != "completed") o += "\n(status: " + *status + ")\n";
    const auto* lim = doc->get("limitations");
    if (lim != nullptr && lim->kind == JsonNode::Kind::array && !lim->items.empty()) {
        o += "\nlimitations:\n";
        for (const auto& l : lim->items) {
            const std::string* c = l.str("code");
            const std::string* d = l.str("detail");
            o += "  - " + (c ? *c : std::string("?")) + (d ? ": " + *d : std::string()) + "\n";
        }
    }
    return o;
}

std::string render_error_text(std::string_view body, int http_status) {
    const auto doc = parse_json(body);
    const auto* err = doc ? doc->get("error") : nullptr;
    if (err == nullptr || err->kind != JsonNode::Kind::object) {
        const std::string* status = doc ? doc->str("status") : nullptr;
        return "agent answered HTTP " + std::to_string(http_status) + (status ? " (status: " + *status + ")" : "") + "\n";
    }
    const std::string* code = err->str("code");
    const std::string* reason = err->str("reason");
    const std::string* msg = err->str("message");
    std::string o = "agent error " + std::to_string(http_status) + ": " + (code ? *code : std::string("unknown"));
    if (reason) o += " (" + *reason + ")";
    if (msg) o += ": " + *msg;
    const auto* r = err->get("retryable");
    if (r != nullptr && r->kind == JsonNode::Kind::boolean && r->b) o += " [retryable]";
    return o + "\n";
}

int run_cli(int argc, char** argv) {
    if (argc < 3) {
        usage(stderr);
        return kExitUsage;
    }
    const std::string sub = argv[2];
    if (sub == "help" || sub == "--help" || sub == "-h") {
        usage(stdout);
        return 0;
    }
    if (sub == "add") return cmd_add(argc, argv);
    if (sub == "remove") return cmd_remove(argc, argv);
    if (sub == "list") return cmd_list(argc, argv);
    if (sub == "status") return cmd_status(argc, argv);
    if (sub == "ask" || sub == "snapshot" || sub == "history" || sub == "capabilities") return cmd_single(sub, argc, argv);
    return usage_error("unknown command '" + sub + "'");
}

}  // namespace osie::remote
