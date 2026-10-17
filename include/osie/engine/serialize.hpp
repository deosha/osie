// SPDX-License-Identifier: Apache-2.0
// JSON serialisation shared by the CLI (--json) and the agent API (src/api),
// so that the two never describe the same object differently.
#pragma once

#include <string>

#include "osie/engine/engine.hpp"
#include "osie/util/json.hpp"

namespace osie::serialize {

/// One analysed series of a temporal answer.
void write_trend(json::Writer& w, const std::string& label, const TrendResult& t);
/// As above; `close` false leaves the object open for the caller to extend.
void write_trend(json::Writer& w, const std::string& label, const TrendResult& t, bool close);

/// One unavailable observation. `detailed`: include the error's full
/// description (CLI); otherwise only its code, since descriptions can name
/// filesystem paths (API).
void write_unavailable(json::Writer& w, const Unavailable& u, bool detailed);

/// `osie ask --json`: the answer with its frame, diagnosis and trends.
std::string answer_json(const Answer& a);

/// `osie snapshot --json`: a one-line summary of a state.
std::string snapshot_summary_json(const SystemState& s);

}  // namespace osie::serialize
