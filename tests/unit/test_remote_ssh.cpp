// SPDX-License-Identifier: Apache-2.0
// The OpenSSH argument vector: exact (golden) contents, forced options that
// a user's ssh_config cannot weaken, the destination after "--", and no way
// for any profile field to add an option or a command.
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "osie/remote/ssh.hpp"

using namespace osie;
using namespace osie::remote;

namespace {

Profile base() {
    Profile p;
    p.name = "web";
    p.destination = "alice@web1.example.com";
    return p;
}

const std::vector<std::string> kForced = {
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=yes",
    "-o", "UpdateHostKeys=no",
    "-o", "CheckHostIP=no",
    "-o", "ExitOnForwardFailure=yes",
    "-o", "StreamLocalBindMask=0177",
    "-o", "StreamLocalBindUnlink=no",
    "-o", "ForwardAgent=no",
    "-o", "ForwardX11=no",
    "-o", "ForwardX11Trusted=no",
    "-o", "Tunnel=no",
    "-o", "SessionType=none",
    "-o", "RemoteCommand=none",
    "-o", "RequestTTY=no",
    "-o", "StdinNull=yes",
    "-o", "EscapeChar=none",
    "-o", "PermitLocalCommand=no",
    "-o", "ControlMaster=no",
    "-o", "ControlPath=none",
    "-o", "ForkAfterAuthentication=no",
    "-o", "AddKeysToAgent=no",
    "-o", "LogLevel=ERROR",
    "-o", "ServerAliveInterval=5",
    "-o", "ServerAliveCountMax=3",
};

std::vector<std::string> concat(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace

TEST(RemoteSsh, GoldenArgvMinimalProfile) {
    auto a = build_ssh_argv("/usr/bin/ssh", base(), "/tmp/osie-remote-0123456789abcdef01234567/fwd.sock", 15);
    ASSERT_TRUE(a.ok()) << a.error().describe();
    const auto want = concat(concat({"/usr/bin/ssh", "-N", "-T"}, kForced),
                             {"-o", "ConnectTimeout=15", "-L",
                              "/tmp/osie-remote-0123456789abcdef01234567/fwd.sock:/run/osie/agent.sock", "--",
                              "alice@web1.example.com"});
    EXPECT_EQ(*a, want);
}

TEST(RemoteSsh, GoldenArgvFullProfile) {
    Profile p = base();
    p.destination = "ops@[2001:db8::7]";
    p.port = 2222;
    p.socket = "/run/osie-test/agent.sock";
    p.identity = "/home/ops/.ssh/id_osie";
    p.known_hosts = "/home/ops/.ssh/known_hosts_osie";
    auto a = build_ssh_argv("/opt/ssh/bin/ssh", p, "/run/user/1000/osie-remote-aa/fwd.sock", 7);
    ASSERT_TRUE(a.ok()) << a.error().describe();
    const auto want = concat(concat({"/opt/ssh/bin/ssh", "-N", "-T"}, kForced),
                             {"-o", "ConnectTimeout=7", "-o", "IdentityFile=/home/ops/.ssh/id_osie", "-o",
                              "IdentitiesOnly=yes", "-o", "UserKnownHostsFile=/home/ops/.ssh/known_hosts_osie", "-p", "2222",
                              "-L", "/run/user/1000/osie-remote-aa/fwd.sock:/run/osie-test/agent.sock", "--",
                              "ops@2001:db8::7"});
    EXPECT_EQ(*a, want);
}

TEST(RemoteSsh, StructuralInvariants) {
    Profile p = base();
    p.port = 22;
    p.identity = "/k/id";
    p.known_hosts = "/k/kh";
    auto a = build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 120);
    ASSERT_TRUE(a.ok());
    const auto& v = *a;
    // "--" is second to last, and the destination is last.
    ASSERT_GE(v.size(), 3u);
    EXPECT_EQ(v[v.size() - 2], "--");
    EXPECT_EQ(v.back(), "alice@web1.example.com");
    EXPECT_EQ(std::count(v.begin(), v.end(), "--"), 1);
    // Every argument before "--" is an option we chose or its value: no
    // destination, no command, no shell.
    for (std::size_t i = 0; i + 2 < v.size(); ++i) {
        EXPECT_EQ(v[i].find('\n'), std::string::npos);
        EXPECT_EQ(v[i].find(' '), std::string::npos) << v[i];
        if (v[i] == "-o") {
            const std::string& o = v[i + 1];
            const auto key = o.substr(0, o.find('='));
            static const std::vector<std::string> kAllowed = {
                "BatchMode", "StrictHostKeyChecking", "UpdateHostKeys", "CheckHostIP", "ExitOnForwardFailure",
                "StreamLocalBindMask", "StreamLocalBindUnlink", "ForwardAgent", "ForwardX11", "ForwardX11Trusted",
                "Tunnel", "SessionType", "RemoteCommand", "RequestTTY", "StdinNull", "EscapeChar", "PermitLocalCommand",
                "ControlMaster", "ControlPath", "ForkAfterAuthentication", "AddKeysToAgent", "LogLevel",
                "ServerAliveInterval", "ServerAliveCountMax", "ConnectTimeout", "IdentityFile", "IdentitiesOnly",
                "UserKnownHostsFile"};
            EXPECT_NE(std::find(kAllowed.begin(), kAllowed.end(), key), kAllowed.end()) << o;
            EXPECT_EQ(o.find("ProxyCommand"), std::string::npos);
            EXPECT_NE(key, "LocalCommand");
            EXPECT_NE(key, "ProxyJump");
        }
    }
    // Never weakened host-key checking, never ClearAllForwardings (it would
    // also clear our -L).
    for (const auto& s : v) {
        EXPECT_NE(s, "StrictHostKeyChecking=no");
        EXPECT_NE(s, "StrictHostKeyChecking=accept-new");
        EXPECT_EQ(s.find("ClearAllForwardings"), std::string::npos);
        EXPECT_EQ(s.find("/dev/null"), std::string::npos) << "known_hosts is never disabled";
    }
    // ConnectTimeout is clamped.
    auto lo = build_ssh_argv("/usr/bin/ssh", base(), "/tmp/d/fwd.sock", 0);
    EXPECT_NE(std::find(lo->begin(), lo->end(), "ConnectTimeout=1"), lo->end());
    auto hi = build_ssh_argv("/usr/bin/ssh", base(), "/tmp/d/fwd.sock", 100000);
    EXPECT_NE(std::find(hi->begin(), hi->end(), "ConnectTimeout=120"), hi->end());
}

TEST(RemoteSsh, InjectionInAnyFieldYieldsNoCommandLine) {
    const std::vector<std::string> hostile = {
        "-oProxyCommand=touch /tmp/x", "-o", "--", "a b", "a\nb", "a'b", "$(id)", "`id`", "a;id", "%h", "${HOME}",
        "ProxyCommand=sh", std::string(4096, 'a'), std::string("a\0b", 3), "a:b", "[a]"};
    for (const auto& h : hostile) {
        Profile p = base();
        p.destination = h;
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "destination " << h;
        p = base();
        p.destination = "alice@" + h;
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "host " << h;
        p = base();
        p.destination = h + "@host";
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "user " << h;
        p = base();
        p.socket = "/run/" + h;
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "socket " << h;
        p = base();
        p.identity = "/k/" + h;
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "identity " << h;
        p = base();
        p.known_hosts = "/k/" + h;
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "known_hosts " << h;
        p = base();
        p.name = h;
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok()) << "name " << h;
        // The local forwarding path is ours, but is checked too.
        EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", base(), "/tmp/" + h + "/fwd.sock", 5).ok()) << "local " << h;
    }
    Profile p = base();
    p.port = 0;
    EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", p, "/tmp/d/fwd.sock", 5).ok());
    EXPECT_FALSE(build_ssh_argv("ssh", base(), "/tmp/d/fwd.sock", 5).ok()) << "ssh path must be absolute";
    EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", base(), "relative/fwd.sock", 5).ok());
    EXPECT_FALSE(build_ssh_argv("/usr/bin/ssh", base(), "/" + std::string(110, 'a'), 5).ok()) << "longer than sun_path";
}

TEST(RemoteSsh, LocateHonoursOsieSshStrictly) {
    const char* saved_c = std::getenv("OSIE_SSH");
    const std::string saved = saved_c ? saved_c : "";
    ::setenv("OSIE_SSH", OSIE_FAKE_SSH, 1);
    auto r = locate_ssh();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r, OSIE_FAKE_SSH);
    ::setenv("OSIE_SSH", "relative/ssh", 1);
    EXPECT_FALSE(locate_ssh().ok()) << "relative OSIE_SSH is refused, never searched";
    ::setenv("OSIE_SSH", "/nonexistent/ssh", 1);
    EXPECT_FALSE(locate_ssh().ok());
    const std::string dir = ::testing::TempDir() + "osie_locate_" + std::to_string(::getpid());
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir + "/ssh");
        f << "#!/bin/sh\n";
    }
    ::chmod((dir + "/ssh").c_str(), 0644);
    ::setenv("OSIE_SSH", (dir + "/ssh").c_str(), 1);
    EXPECT_FALSE(locate_ssh().ok()) << "not executable";
    ::setenv("OSIE_SSH", dir.c_str(), 1);
    EXPECT_FALSE(locate_ssh().ok()) << "a directory";
    ::unsetenv("OSIE_SSH");
    // PATH: relative entries are skipped.
    const char* old_path_c = std::getenv("PATH");
    const std::string old_path = old_path_c ? old_path_c : "";
    ::chmod((dir + "/ssh").c_str(), 0755);
    ::setenv("PATH", (std::string(".:relative:") + dir).c_str(), 1);
    auto p = locate_ssh();
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(*p, dir + "/ssh");
    ::setenv("PATH", old_path.c_str(), 1);
    if (saved_c) ::setenv("OSIE_SSH", saved.c_str(), 1);
    std::filesystem::remove_all(dir);
}
