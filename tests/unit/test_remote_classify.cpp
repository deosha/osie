// SPDX-License-Identifier: Apache-2.0
// Classification of OpenSSH failures from its exit status and standard
// error. The fixtures are messages of OpenSSH 8.9-9.9 (as printed with
// LogLevel=ERROR), so a change in wording shows up here first.
#include <gtest/gtest.h>

#include "osie/remote/errors.hpp"
#include "osie/remote/http.hpp"

using namespace osie::remote;

namespace {

RemoteErrorCode code_of(const std::string& err) { return classify_ssh_exit(255, err).code; }

const char* kMismatch =
    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
    "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\r\n"
    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
    "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\r\n"
    "Someone could be eavesdropping on you right now (man-in-the-middle attack)!\r\n"
    "It is also possible that a host key has just been changed.\r\n"
    "The fingerprint for the ED25519 key sent by the remote host is\r\n"
    "SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG.\r\n"
    "Please contact your system administrator.\r\n"
    "Add correct host key in /home/u/.ssh/known_hosts to get rid of this message.\r\n"
    "Offending ED25519 key in /home/u/.ssh/known_hosts:1\r\n"
    "Host key for [127.0.0.1]:2222 has changed and you have requested strict checking.\r\n"
    "Host key verification failed.\r\n";

}  // namespace

TEST(RemoteClassify, HostKeys) {
    EXPECT_EQ(code_of("No ED25519 host key is known for [127.0.0.1]:2222 and you have requested strict checking.\r\n"
                      "Host key verification failed.\r\n"),
              RemoteErrorCode::host_key_unknown);
    EXPECT_EQ(code_of("Host key verification failed.\r\n"), RemoteErrorCode::host_key_unknown);
    EXPECT_EQ(code_of(kMismatch), RemoteErrorCode::host_key_mismatch) << "mismatch wins over 'verification failed'";
    EXPECT_EQ(code_of("@@@@@@@\r\n@       WARNING: REVOKED HOST KEY DETECTED!               @\r\n"
                      "Host key verification failed.\r\n"),
              RemoteErrorCode::host_key_mismatch);
    EXPECT_EQ(code_of("@@@ WARNING: POSSIBLE DNS SPOOFING DETECTED! @@@\r\nHost key verification failed.\r\n"),
              RemoteErrorCode::host_key_mismatch);
    const auto e = classify_ssh_exit(255, kMismatch);
    EXPECT_FALSE(retryable(e.code));
    EXPECT_EQ(e.ssh_exit, 255);
    EXPECT_NE(e.message.find("known_hosts"), std::string::npos);
}

TEST(RemoteClassify, Authentication) {
    for (const char* s : {"alice@127.0.0.1: Permission denied (publickey).\r\n",
                          "alice@host: Permission denied (publickey,password,keyboard-interactive).\r\n",
                          "Received disconnect from 10.0.0.1 port 22:2: Too many authentication failures\r\n"
                          "Disconnected from 10.0.0.1 port 22\r\n",
                          "Load key \"/home/u/.ssh/id_bad\": invalid format\r\nu@h: Permission denied (publickey).\r\n"})
        EXPECT_EQ(code_of(s), RemoteErrorCode::authentication_failed) << s;
}

TEST(RemoteClassify, ConnectionAndTimeouts) {
    for (const char* s : {"ssh: Could not resolve hostname nonexistent.invalid: Name or service not known\r\n",
                          "ssh: Could not resolve hostname x: nodename nor servname provided, or not known\r\n",
                          "ssh: connect to host 127.0.0.1 port 2299: Connection refused\r\n",
                          "ssh: connect to host 10.0.0.9 port 22: No route to host\r\n",
                          "kex_exchange_identification: read: Connection reset by peer\r\n"
                          "Connection reset by 127.0.0.1 port 2222\r\n",
                          "kex_exchange_identification: Connection closed by remote host\r\n", "", "something new\r\n"})
        EXPECT_EQ(code_of(s), RemoteErrorCode::connection_failed) << s;
    for (const char* s : {"ssh: connect to host 10.255.255.1 port 22: Connection timed out\r\n",
                          "ssh: connect to host 10.255.255.1 port 22: Operation timed out\r\n",
                          "Connection timed out during banner exchange\r\nConnection to 127.0.0.1 port 2223 timed out\r\n",
                          "Timeout, server 10.0.0.1 not responding.\r\n"})
        EXPECT_EQ(code_of(s), RemoteErrorCode::timeout) << s;
    EXPECT_TRUE(retryable(RemoteErrorCode::connection_failed));
    EXPECT_TRUE(retryable(RemoteErrorCode::timeout));
}

TEST(RemoteClassify, IncompatibleOpenSsh) {
    EXPECT_EQ(code_of("command-line line 0: Bad configuration option: sessiontype\r\n"), RemoteErrorCode::ssh_unavailable);
    EXPECT_EQ(code_of("command-line line 0: Unsupported option \"forkafterauthentication\"\r\n"),
              RemoteErrorCode::ssh_unavailable);
}

TEST(RemoteClassify, LocalForwardFailure) {
    const auto e = classify_ssh_exit(255, "unix_listener: cannot bind to path /tmp/x/fwd.sock: Address already in use\r\n"
                                          "Could not request local forwarding.\r\n");
    EXPECT_EQ(e.code, RemoteErrorCode::connection_failed);
    EXPECT_NE(e.message.find("local forwarding"), std::string::npos);
}

TEST(RemoteClassify, ChannelRefusalIsNeutral) {
    // OpenSSH prints the same line for a missing socket, EACCES and
    // AllowStreamLocalForwarding=no: the message names all three.
    const auto e = classify_channel_eof("channel 2: open failed: connect failed: open failed\r\n");
    EXPECT_EQ(e.code, RemoteErrorCode::remote_socket_unavailable);
    EXPECT_NE(e.message.find("api_enabled"), std::string::npos);
    EXPECT_NE(e.message.find("api_group"), std::string::npos);
    EXPECT_NE(e.message.find("AllowStreamLocalForwarding"), std::string::npos);
    EXPECT_EQ(classify_channel_eof("").code, RemoteErrorCode::remote_socket_unavailable);
}

TEST(RemoteClassify, StderrIsSanitisedAndBounded) {
    const std::string hostile = "\x1b]0;owned\x07\x1b[2Jline1\r\nline2\x9b" + std::string(5000, 'x');
    const auto s = sanitize_stderr(hostile);
    EXPECT_LE(s.size(), 2048u);
    for (unsigned char c : s) EXPECT_TRUE(c == '\n' || (c >= 0x20 && c < 0x7f)) << int(c);
    EXPECT_EQ(s.find('\x1b'), std::string::npos);
    EXPECT_EQ(s.find('\r'), std::string::npos);
    const auto e = classify_ssh_exit(255, hostile);
    EXPECT_EQ(e.ssh_stderr.find('\x1b'), std::string::npos);
}

TEST(RemoteClassify, EnvelopeMatchesTheSchemaShape) {
    RemoteError e = classify_ssh_exit(255, "Host key verification failed.\n");
    const auto env = error_envelope(e, "prod");
    auto v = parse_json(env);
    ASSERT_TRUE(v) << env;
    EXPECT_EQ(*v->str("schema_version"), "1.0");
    EXPECT_EQ(*v->str("remote"), "prod");
    const auto* re = v->get("remote_error");
    ASSERT_NE(re, nullptr);
    EXPECT_EQ(*re->str("code"), "host_key_unknown");
    EXPECT_EQ(re->get("retryable")->kind, JsonNode::Kind::boolean);
    EXPECT_EQ(re->get("ssh_exit_status")->text, "255");
    EXPECT_EQ(v->get("action_executed")->kind, JsonNode::Kind::boolean);
    EXPECT_EQ(v->get("error"), nullptr) << "never mistaken for an agent error envelope";
    for (auto c : {RemoteErrorCode::host_key_unknown, RemoteErrorCode::host_key_mismatch,
                   RemoteErrorCode::authentication_failed, RemoteErrorCode::connection_failed, RemoteErrorCode::timeout,
                   RemoteErrorCode::remote_socket_unavailable, RemoteErrorCode::protocol_error,
                   RemoteErrorCode::ssh_unavailable, RemoteErrorCode::invalid_profile, RemoteErrorCode::interrupted})
        EXPECT_FALSE(to_string(c).empty());
}
