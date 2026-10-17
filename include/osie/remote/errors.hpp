// SPDX-License-Identifier: Apache-2.0
// Transport failures of `osie remote` (docs/REMOTE.md, "Errors").
//
// A transport failure means the agent did not answer: OpenSSH is missing,
// the host key is unknown or has changed, authentication failed, the
// connection or the forwarded agent socket failed, the deadline passed, or
// the reply was not a well-formed agent response. It is reported as a
// `remote_error` envelope (docs/schemas/remote_error.schema.json) and exit
// status 3. An answer of the agent, including an API error, is never one.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace osie::remote {

/// Stable codes. Never renamed or removed; new codes may be added.
enum class RemoteErrorCode : std::uint8_t {
    host_key_unknown,
    host_key_mismatch,
    authentication_failed,
    connection_failed,
    timeout,
    remote_socket_unavailable,
    protocol_error,
    ssh_unavailable,
    invalid_profile,
    interrupted,  // a signal (SIGINT, SIGTERM, SIGHUP) ended the command
};
std::string_view to_string(RemoteErrorCode c) noexcept;

/// Whether retrying the same command later may succeed.
bool retryable(RemoteErrorCode c) noexcept;

struct RemoteError {
    RemoteErrorCode code{RemoteErrorCode::connection_failed};
    std::string message;              // one sentence for a person; no secrets
    std::optional<int> ssh_exit;      // exit status of ssh, if it exited
    std::optional<int> ssh_signal;    // signal that ended ssh, if any
    std::string ssh_stderr;           // bounded, control characters replaced
};

/// Classifies an `ssh` that exited before the forward was usable, from its
/// exit status and (bounded) standard error. OpenSSH reports every failure
/// with status 255; the reason is only in its messages, which are matched
/// against fixed patterns (tests: RemoteClassify.*).
RemoteError classify_ssh_exit(int exit_status, std::string_view stderr_text);

/// Classifies the forwarded connection closing before any byte of a reply.
/// OpenSSH reports a refused channel (no socket at the remote path, no
/// permission, or forwarding disabled by the server) only as EOF on the local
/// socket and one identical "open failed" line for all three causes; ssh
/// itself keeps running (ExitOnForwardFailure does not apply per connection).
RemoteError classify_channel_eof(std::string_view stderr_text);

/// Replaces bytes outside printable ASCII (and tabs) with '?' and truncates
/// to `max` bytes, so terminal escape sequences from a remote banner or a
/// hostile server never reach the user's terminal.
std::string sanitize_stderr(std::string_view s, std::size_t max = 2048);

/// The `remote_error` envelope, one line of compact JSON.
std::string error_envelope(const RemoteError& e, std::string_view remote_name);

}  // namespace osie::remote
