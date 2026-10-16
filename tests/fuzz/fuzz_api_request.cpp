// SPDX-License-Identifier: Apache-2.0
// libFuzzer harness for the local API's request path: HTTP head parsing,
// body framing, routing, query and JSON validation, and the diagnosis
// pipeline over a fixed mock snapshot. Any crash, sanitizer report or
// non-JSON response body is a finding.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "osie/api/api.hpp"
#include "osie/api/json_in.hpp"
#include "support/agent_fixture.hpp"
#include "support/json_value.hpp"

namespace {

osie::api::DiagnosticApi& api() {
    static auto rt = [] {
        auto r = std::make_shared<osie::agent::AgentRuntime>(osie::test::machine(), osie::test::fast_config());
        r->collect_now();
        r->collect_now();
        return r;
    }();
    static osie::api::DiagnosticApi a(rt, [] {
        osie::api::ApiSettings s;
        s.max_evidence_age = osie::Millis{24LL * 3600 * 1000};  // the fixed snapshot never goes stale
        return s;
    }());
    return a;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace osie::api;
    const std::string_view in(reinterpret_cast<const char*>(data), size);
    (void)parse_flat_object(in);
    (void)parse_query(in.substr(0, 256));
    HeadLimits lim;
    lim.max_body_bytes = 16 * 1024;
    auto h = parse_head(in, lim);
    if (h.state != HeadParse::State::complete) return 0;
    HttpRequest req = h.request;
    const std::size_t have = in.size() - req.head_bytes;
    req.body = std::string(in.substr(req.head_bytes, std::min(have, req.content_length.value_or(0))));
    osie::api::PeerCredentials peer;
    peer.known = true;
    peer.uid = (size % 2) ? 0 : osie::test::kAlice;
    const Access access = (size % 3 == 0) ? Access::health : (size % 2 ? Access::full : Access::diagnostics);
    const auto resp = api().handle(req, peer, access);
    // Every response is a JSON object with a schema version.
    try {
        const auto v = osie::test::JsonReader::parse(resp.body);
        if (!v.has("schema_version")) std::abort();
    } catch (...) {
        std::abort();
    }
    return 0;
}
