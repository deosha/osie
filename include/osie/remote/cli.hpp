// SPDX-License-Identifier: Apache-2.0
// `osie remote ...` (docs/REMOTE.md). Exit status:
//   0  the agent answered with 2xx
//   1  the agent answered with an API error (its body is passed through)
//   2  usage error, unknown or invalid profile
//   3  transport failure (a remote_error envelope)
#pragma once

#include <string>
#include <string_view>

namespace osie::remote {

/// Runs `osie remote <args>`; argv[0] is the program, argv[1] is "remote".
int run_cli(int argc, char** argv);

/// Text rendering of a POST /v1/diagnose body (2xx). Reads only `status`,
/// `answer.text` and `limitations[].code/detail`; ignores everything else.
std::string render_answer_text(std::string_view body);

/// Text rendering of an API error envelope. Reads only `error.*` (and
/// `status`, for a readiness answer that is not an error envelope).
std::string render_error_text(std::string_view body, int http_status);

}  // namespace osie::remote
