// SPDX-License-Identifier: Apache-2.0
#include "osie/remote/errors.hpp"

#include <array>

#include "osie/util/json.hpp"

namespace osie::remote {
namespace {

bool has(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

template <std::size_t N>
bool has_any(std::string_view hay, const std::array<std::string_view, N>& needles) {
    for (auto n : needles)
        if (has(hay, n)) return true;
    return false;
}

/// First non-empty line of ssh's output, for a fallback message.
std::string first_line(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t j = std::min(s.find('\n', i), s.size());
        std::string_view line = s.substr(i, j - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty()) return sanitize_stderr(line, 200);
        i = j + 1;
    }
    return {};
}

}  // namespace

std::string_view to_string(RemoteErrorCode c) noexcept {
    switch (c) {
        case RemoteErrorCode::host_key_unknown: return "host_key_unknown";
        case RemoteErrorCode::host_key_mismatch: return "host_key_mismatch";
        case RemoteErrorCode::authentication_failed: return "authentication_failed";
        case RemoteErrorCode::connection_failed: return "connection_failed";
        case RemoteErrorCode::timeout: return "timeout";
        case RemoteErrorCode::remote_socket_unavailable: return "remote_socket_unavailable";
        case RemoteErrorCode::protocol_error: return "protocol_error";
        case RemoteErrorCode::ssh_unavailable: return "ssh_unavailable";
        case RemoteErrorCode::invalid_profile: return "invalid_profile";
        case RemoteErrorCode::interrupted: return "interrupted";
    }
    return "connection_failed";
}

bool retryable(RemoteErrorCode c) noexcept {
    switch (c) {
        case RemoteErrorCode::connection_failed:
        case RemoteErrorCode::timeout:
        case RemoteErrorCode::remote_socket_unavailable:
        case RemoteErrorCode::interrupted:
            return true;
        default:
            return false;
    }
}

std::string sanitize_stderr(std::string_view s, std::size_t max) {
    std::string o;
    o.reserve(std::min(s.size(), max));
    for (unsigned char c : s) {
        if (o.size() >= max) break;
        if (c == '\r') continue;
        if (c == '\n' || (c >= 0x20 && c < 0x7f)) o += static_cast<char>(c);
        else o += '?';
    }
    while (!o.empty() && o.back() == '\n') o.pop_back();
    return o;
}

RemoteError classify_ssh_exit(int exit_status, std::string_view err) {
    RemoteError e;
    e.ssh_exit = exit_status;
    e.ssh_stderr = sanitize_stderr(err);
    // Order matters: a changed key also prints "Host key verification failed."
    static constexpr std::array<std::string_view, 5> kMismatch = {
        "REMOTE HOST IDENTIFICATION HAS CHANGED", "has changed and you have requested strict checking",
        "REVOKED HOST KEY", "POSSIBLE DNS SPOOFING", "Host key for "};
    static constexpr std::array<std::string_view, 3> kUnknown = {
        "host key is known for", "Host key verification failed", "No matching host key"};
    static constexpr std::array<std::string_view, 6> kAuth = {
        "Permission denied (", "Too many authentication failures", "Authentication failed",
        "No more authentication methods", "no mutual signature algorithm", "Load key"};
    static constexpr std::array<std::string_view, 5> kTimeout = {
        "Connection timed out", "Operation timed out", "timed out during banner exchange", "Connection timeout",
        "Timeout, server"};
    static constexpr std::array<std::string_view, 6> kOption = {
        "Bad configuration option", "Unsupported option", "unknown option", "illegal option", "Unknown option",
        "keyword is not supported"};
    static constexpr std::array<std::string_view, 3> kLocalForward = {
        "Could not request local forwarding", "unix_listener", "cannot listen to path"};
    const bool mismatch_banner = has_any(err, kMismatch) &&
                                 (has(err, "has changed") || has(err, "REMOTE HOST IDENTIFICATION") ||
                                  has(err, "REVOKED") || has(err, "SPOOFING"));
    if (mismatch_banner) {
        e.code = RemoteErrorCode::host_key_mismatch;
        e.message = "The remote host key does not match the known_hosts entry; the connection was refused. "
                    "Verify the new key out of band before updating known_hosts.";
    } else if (has_any(err, kUnknown)) {
        e.code = RemoteErrorCode::host_key_unknown;
        e.message = "The remote host key is not in known_hosts; OSIE never accepts new host keys. "
                    "Verify the key out of band and add it to known_hosts.";
    } else if (has_any(err, kAuth)) {
        e.code = RemoteErrorCode::authentication_failed;
        e.message = "SSH authentication failed (OSIE runs ssh in batch mode: no password or passphrase prompts).";
    } else if (has_any(err, kOption)) {
        e.code = RemoteErrorCode::ssh_unavailable;
        e.message = "The installed OpenSSH client does not support the options OSIE requires (OpenSSH 8.7 or later).";
    } else if (has_any(err, kTimeout)) {
        e.code = RemoteErrorCode::timeout;
        e.message = "The SSH connection timed out.";
    } else if (has_any(err, kLocalForward)) {
        e.code = RemoteErrorCode::connection_failed;
        e.message = "OpenSSH could not create the local forwarding socket.";
    } else {
        e.code = RemoteErrorCode::connection_failed;
        const std::string line = first_line(err);
        e.message = line.empty() ? "ssh exited with status " + std::to_string(exit_status) + " before the forward was ready."
                                 : "SSH connection failed: " + line;
    }
    return e;
}

RemoteError classify_channel_eof(std::string_view err) {
    // OpenSSH prints the same "open failed" line whether the socket is
    // missing, the login user may not connect to it, or the server forbids
    // Unix-socket forwarding; only the server's log tells them apart. So the
    // message lists the causes instead of guessing one.
    RemoteError e;
    e.code = RemoteErrorCode::remote_socket_unavailable;
    e.ssh_stderr = sanitize_stderr(err);
    e.message = "The remote agent socket could not be opened through SSH. Possible causes: no osie-agent is "
                "listening there (api_enabled = true?), the SSH login user may not use the socket (not in "
                "api_group), or the SSH server does not allow Unix-socket forwarding (AllowStreamLocalForwarding).";
    return e;
}

std::string error_envelope(const RemoteError& e, std::string_view remote_name) {
    json::Writer w;
    w.begin_object().kv("schema_version", "1.0");
    if (!remote_name.empty()) w.kv("remote", remote_name);
    w.key("remote_error").begin_object().kv("code", to_string(e.code)).kv("message", e.message)
        .kv("retryable", retryable(e.code));
    if (e.ssh_exit) w.kv("ssh_exit_status", *e.ssh_exit);
    if (e.ssh_signal) w.kv("ssh_signal", *e.ssh_signal);
    if (!e.ssh_stderr.empty()) w.kv("ssh_stderr", e.ssh_stderr);
    w.end_object().kv("action_executed", false).end_object();
    return w.take();
}

}  // namespace osie::remote
