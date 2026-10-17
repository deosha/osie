// SPDX-License-Identifier: Apache-2.0
// libFuzzer harness for what `osie remote` reads from another host: the HTTP
// response parser, the JSON reader, the re-indenter and the text renderers.
// Any crash or sanitizer report is a finding, and so is a broken invariant:
// - a complete reply's body is exactly the input after the head (bytes are
//   passed through, never re-serialised), and parses as a JSON object;
// - the re-indented form of valid JSON is valid JSON again;
// - the classification of ssh output always yields a stable code.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "osie/remote/cli.hpp"
#include "osie/remote/errors.hpp"
#include "osie/remote/http.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace osie::remote;
    const std::string_view in(reinterpret_cast<const char*>(data), size);

    for (bool eof : {false, true}) {
        const auto r = parse_reply(in, eof, 64 * 1024);
        if (r.state == ReplyParse::State::complete) {
            if (r.reply.body.size() > in.size() || in.substr(in.size() - r.reply.body.size()) != r.reply.body) std::abort();
            const auto doc = parse_json(r.reply.body);
            if (!doc || doc->kind != JsonNode::Kind::object) std::abort();
            (void)render_answer_text(r.reply.body);
            (void)render_error_text(r.reply.body, r.reply.status);
        }
    }

    if (const auto doc = parse_json(in)) {
        const std::string pretty = pretty_json(in);
        if (!parse_json(pretty)) std::abort();
        (void)render_answer_text(in);
        (void)render_error_text(in, 503);
    }

    const auto e = classify_ssh_exit(255, in);
    if (to_string(e.code).empty()) std::abort();
    const auto c = classify_channel_eof(in);
    if (c.code != RemoteErrorCode::remote_socket_unavailable) std::abort();
    for (unsigned char ch : sanitize_stderr(in))
        if (ch != '\n' && (ch < 0x20 || ch >= 0x7f)) std::abort();
    (void)error_envelope(e, "fuzz");
    return 0;
}
