// SPDX-License-Identifier: Apache-2.0
// Remote profiles: the grammar of every field (each ends up in ssh's argument
// vector), the strict file format, and the file's ownership, mode, symlink
// and atomic-replacement rules.
#include <gtest/gtest.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "osie/remote/profile.hpp"

using namespace osie;
using namespace osie::remote;

namespace {

std::string tmpdir(const std::string& name) {
    std::string base = ::testing::TempDir();
    if (base.empty() || base.back() != '/') base += '/';
    const std::string d = base + "osrp" + std::to_string(::getpid()) + "_" + name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    ::chmod(d.c_str(), 0700);
    return d;
}

std::string read_file(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Profile good() {
    Profile p;
    p.name = "prod-db.1";
    p.destination = "alice@db1.example.com";
    p.port = 2222;
    p.socket = "/run/osie/agent.sock";
    p.identity = "/home/alice/.ssh/id_ed25519";
    p.known_hosts = "/home/alice/.ssh/known_hosts";
    p.timeout_s = 20;
    return p;
}

// Strings that must never reach ssh's command line through any field.
const std::vector<std::string> kHostile = {
    "-oProxyCommand=touch /tmp/pwned",
    "-o ProxyCommand=x",
    "--",
    "-",
    "a b",
    "a\tb",
    "a\nb",
    "a\rb",
    std::string("a\0b", 3),
    "a'b",
    "a\"b",
    "$(id)",
    "`id`",
    "a;id",
    "a|id",
    "a&id",
    "a>b",
    "a<b",
    "%h",
    "a%rb",
    "${HOME}",
    "$HOME",
    "~/x",
    "a\\b",
    "a*b",
    "a?b",
    "a!b",
    "a#b",
    "ProxyCommand=sh -c id",
    "a,b",
    "a=b",
    "\x7f",
    "caf\xc3\xa9",
    std::string(5000, 'a'),
};

}  // namespace

TEST(RemoteProfile, ValidFieldsAreAccepted) {
    EXPECT_FALSE(check_profile(good()));
    for (const std::string n : {std::string("a"), std::string("prod"), std::string("Prod-DB_2.eu"), std::string("0x"), std::string(64, 'n')})
        EXPECT_FALSE(check_name(n)) << n;
    for (const char* d : {"host", "db1.example.com", "alice@host", "_svc@host", "a.b-c_d@h-1.x", "10.0.0.1", "u@192.168.1.20",
                          "u@[::1]", "[2001:db8::1]", "u@[::ffff:10.0.0.1]", "my_alias", "h--x"})
        EXPECT_FALSE(check_destination(d)) << d;
    for (const char* s : {"/run/osie/agent.sock", "/a", "/var/run/osie-1/x.sock"}) EXPECT_FALSE(check_socket_path(s)) << s;
    for (const char* s : {"/home/u/.ssh/id_ed25519", "/x/known_hosts+old", "/a/b@c"}) EXPECT_FALSE(check_file_path(s)) << s;
    EXPECT_FALSE(check_port(1));
    EXPECT_FALSE(check_port(65535));
    EXPECT_FALSE(check_timeout(1));
    EXPECT_FALSE(check_timeout(120));
}

TEST(RemoteProfile, HostileStringsAreRefusedInEveryField) {
    for (const auto& h : kHostile) {
        EXPECT_TRUE(check_name(h)) << "name: " << h;
        EXPECT_TRUE(check_destination(h)) << "destination: " << h;
        EXPECT_TRUE(check_destination("alice@" + h)) << "host: " << h;
        EXPECT_TRUE(check_destination(h + "@host")) << "user: " << h;
        EXPECT_TRUE(check_socket_path(h)) << "socket: " << h;
        EXPECT_TRUE(check_socket_path("/run/" + h)) << "socket: " << h;
        EXPECT_TRUE(check_file_path(h)) << "path: " << h;
        if (h.find('/') == std::string::npos) EXPECT_TRUE(check_file_path("/home/" + h)) << "path: /home/" << h;
    }
}

TEST(RemoteProfile, DestinationGrammarEdges) {
    for (const char* d : {"", "-host", "-oProxyCommand=x", "@host", "user@", "a@b@c", "user@-host", "user@host-", "user@.host",
                          "user@host.", "user@ho..st", "1user@host", "user@[::1", "user@::1", "user@[::1]:22",
                          "user@[fe80::1%eth0]", "user@[zz::1]", "user@999.1.1.1", "user@1.2.3", "user@host:22",
                          "ssh://user@host", "user@host/x", "u@[]", "user name@host"})
        EXPECT_TRUE(check_destination(d)) << d;
    EXPECT_TRUE(check_destination("u@" + std::string(254, 'a')));
    EXPECT_TRUE(check_destination("u@" + std::string(64, 'a') + ".com")) << "label longer than 63";
    EXPECT_TRUE(check_destination(std::string(33, 'u') + "@host")) << "user longer than 32";
}

TEST(RemoteProfile, PathGrammarEdges) {
    for (const char* s : {"", "relative/agent.sock", "agent.sock", "/", "/run//osie.sock", "/run/osie/", "/run/./x", "/run/../x",
                          "/run/-x", "/run/osie:agent.sock", "/run/[x]"})
        EXPECT_TRUE(check_socket_path(s)) << s;
    EXPECT_TRUE(check_socket_path("/" + std::string(100, 'a'))) << "over 100 bytes";
    EXPECT_FALSE(check_socket_path("/" + std::string(99, 'a')));
    EXPECT_TRUE(check_file_path("/" + std::string(1024, 'a')));
    EXPECT_TRUE(check_socket_path("/run/a+b")) << "'+' is not allowed in socket paths";
    EXPECT_TRUE(check_port(0));
    EXPECT_TRUE(check_port(65536));
    EXPECT_TRUE(check_timeout(0));
    EXPECT_TRUE(check_timeout(121));
}

TEST(RemoteProfile, Ipv6IsUnbracketedForOpenSsh) {
    EXPECT_EQ(ssh_destination("alice@[2001:db8::1]"), "alice@2001:db8::1");
    EXPECT_EQ(ssh_destination("[::1]"), "::1");
    EXPECT_EQ(ssh_destination("alice@host"), "alice@host");
    EXPECT_EQ(ssh_destination("host"), "host");
}

TEST(RemoteProfile, DecimalParsingIsStrict) {
    EXPECT_EQ(parse_decimal("22"), 22);
    EXPECT_EQ(parse_decimal("0010"), 10);
    for (const char* s : {"", "-1", "+1", " 1", "1 ", "1e3", "0x10", "1.5", "1234567890", "١"}) EXPECT_FALSE(parse_decimal(s)) << s;
}

TEST(RemoteProfile, FileFormatRoundTrips) {
    Profile a = good();
    Profile b;
    b.name = "b";
    b.destination = "host";
    const std::vector<Profile> ps = {a, b};
    const std::string text = serialize_profiles(ps);
    auto back = parse_profiles(text);
    ASSERT_TRUE(back.ok()) << back.error().describe();
    ASSERT_EQ(back->size(), 2u);
    EXPECT_EQ((*back)[0], a);
    EXPECT_EQ((*back)[1], b);
    EXPECT_FALSE((*back)[1].port.has_value());
    EXPECT_EQ((*back)[1].socket, "/run/osie/agent.sock");
    EXPECT_EQ((*back)[1].timeout_s, kDefaultTimeoutS);
}

TEST(RemoteProfile, FileFormatIsStrict) {
    const std::string ok = "[a]\ndestination = host\n";
    ASSERT_TRUE(parse_profiles(ok).ok());
    ASSERT_TRUE(parse_profiles("# only a comment\n\n").ok());
    for (const std::string bad : {
             "destination = host\n",                             // outside a section
             "[a]\n",                                            // no destination
             "[a]\ndestination = host\n[a]\ndestination = h\n",  // duplicate section
             "[a]\ndestination = host\ndestination = h\n",       // duplicate key
             "[a]\ndestination = host\nproxy_command = x\n",     // unknown key
             "[a]\ndestination = host\nProxyCommand = x\n",
             "[a]\ndestination = -oProxyCommand=x\n",
             "[a]\ndestination = host\nport = 0\n",
             "[a]\ndestination = host\nport = 22 23\n",
             "[a]\ndestination = host\nsocket = relative\n",
             "[a]\ndestination = host\nidentity = ~/.ssh/id\n",
             "[a]\ndestination = host\ntimeout = 500\n",
             "[a]\ndestination = host\r\n",                      // CR
             "[a]\ndestination = host\x01\n",
             "[a]\ndestination = h\xc3\xa9\n",
             "[-a]\ndestination = host\n",
             "[a b]\ndestination = host\n",
             "[a\ndestination = host\n",
             "[a]\ndestination host\n",
             "[a]\ndestination = host # comment\n",
         })
        EXPECT_FALSE(parse_profiles(bad).ok()) << bad;
    std::string many;
    for (std::size_t i = 0; i <= kMaxProfiles; ++i) many += "[p" + std::to_string(i) + "]\ndestination = h\n";
    EXPECT_FALSE(parse_profiles(many).ok()) << "more than kMaxProfiles";
    EXPECT_FALSE(parse_profiles(std::string(kMaxProfileFileBytes + 1, '#')).ok());
    // Errors name the line, never echo the value.
    auto r = parse_profiles("[a]\ndestination = $(touch /tmp/x)\n");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error().message.find("line 2"), std::string::npos);
    EXPECT_EQ(r.error().message.find("touch"), std::string::npos);
}

TEST(RemoteProfile, StoreCreatesPrivateDirectoryAndFile) {
    const std::string base = tmpdir("create");
    ProfileStore store(base + "/cfg/osie");
    auto empty = store.load();
    ASSERT_TRUE(empty.ok()) << empty.error().describe();
    EXPECT_TRUE(empty->empty()) << "a missing directory is an empty list";
    ASSERT_TRUE(store.update([](std::vector<Profile>& ps) -> Result<void> {
        ps.push_back(good());
        return {};
    }).ok());
    struct stat st {};
    ASSERT_EQ(::stat((base + "/cfg/osie").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);
    ASSERT_EQ(::stat((base + "/cfg").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);
    ASSERT_EQ(::stat(store.file().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0600u);
    EXPECT_EQ(st.st_uid, ::geteuid());
    auto back = store.load();
    ASSERT_TRUE(back.ok());
    ASSERT_EQ(back->size(), 1u);
    EXPECT_EQ((*back)[0], good());
    // No temporary file is left behind.
    std::size_t entries = 0;
    for (const auto& e : std::filesystem::directory_iterator(store.dir())) {
        (void)e;
        ++entries;
    }
    EXPECT_EQ(entries, 1u);
    // An edit that fails writes nothing.
    ASSERT_FALSE(store.update([](std::vector<Profile>& ps) -> Result<void> {
        ps.clear();
        return Error{Errc::invalid_argument, "no", 0};
    }).ok());
    EXPECT_EQ(store.load()->size(), 1u);
    // An edit that produces an invalid profile writes nothing.
    ASSERT_FALSE(store.update([](std::vector<Profile>& ps) -> Result<void> {
        ps[0].destination = "-oProxyCommand=x";
        return {};
    }).ok());
    EXPECT_EQ((*store.load())[0], good());
    std::filesystem::remove_all(base);
}

TEST(RemoteProfile, StoreRefusesLooseModesSymlinksAndOddFiles) {
    const std::string base = tmpdir("modes");
    ProfileStore store(base + "/osie");
    ASSERT_TRUE(store.update([](std::vector<Profile>& ps) -> Result<void> {
        ps.push_back(good());
        return {};
    }).ok());
    ::chmod(store.file().c_str(), 0640);
    EXPECT_FALSE(store.load().ok()) << "group-readable file";
    ::chmod(store.file().c_str(), 0604);
    EXPECT_FALSE(store.load().ok()) << "world-readable file";
    ::chmod(store.file().c_str(), 0400);
    EXPECT_TRUE(store.load().ok()) << "stricter than 0600 is fine";
    ::chmod(store.file().c_str(), 0600);
    ::chmod(store.dir().c_str(), 0750);
    EXPECT_FALSE(store.load().ok()) << "group-accessible directory";
    EXPECT_FALSE(store.update([](std::vector<Profile>&) -> Result<void> { return {}; }).ok());
    ::chmod(store.dir().c_str(), 0700);
    ::chmod(base.c_str(), 0777);
    EXPECT_FALSE(store.load().ok()) << "world-writable parent (not sticky)";
    ::chmod(base.c_str(), 0700);

    // A symlink in place of the file is refused, not followed.
    const std::string target = base + "/elsewhere.conf";
    std::filesystem::copy_file(store.file(), target);
    ::chmod(target.c_str(), 0600);
    std::filesystem::remove(store.file());
    std::filesystem::create_symlink(target, store.file());
    EXPECT_FALSE(store.load().ok()) << "symlinked file";
    // update() replaces the symlink entry itself, never writes through it.
    const std::string before = read_file(target);
    std::filesystem::remove(store.file());
    std::filesystem::create_symlink(target, store.file());
    EXPECT_FALSE(store.update([](std::vector<Profile>&) -> Result<void> { return {}; }).ok());
    EXPECT_EQ(read_file(target), before);
    std::filesystem::remove(store.file());

    // A FIFO or directory at the path is refused without blocking.
    ASSERT_EQ(::mkfifo(store.file().c_str(), 0600), 0);
    EXPECT_FALSE(store.load().ok());
    std::filesystem::remove(store.file());
    std::filesystem::create_directory(store.file());
    EXPECT_FALSE(store.load().ok());
    std::filesystem::remove(store.file());

    // An oversized or malformed file is refused.
    {
        std::ofstream f(store.file());
        f << std::string(kMaxProfileFileBytes + 10, '#');
    }
    ::chmod(store.file().c_str(), 0600);
    EXPECT_FALSE(store.load().ok());
    {
        std::ofstream f(store.file());
        f << "[a]\ndestination = -oProxyCommand=x\n";
    }
    EXPECT_FALSE(store.load().ok());
    std::filesystem::remove_all(base);
}

TEST(RemoteProfile, ConcurrentUpdatesLoseNothing) {
    // 32 `osie remote add` processes at once against one file.
    const std::string base = tmpdir("concurrent");
    const std::string xdg = "XDG_CONFIG_HOME=" + base;
    constexpr int kProcs = 32;
    std::vector<pid_t> kids;
    for (int k = 0; k < kProcs; ++k) {
        const std::string name = "p" + std::to_string(k);
        std::vector<std::string> args = {OSIE_CLI, "remote", "add", name, "host"};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        std::string env_path = "PATH=/usr/bin:/bin";
        char* envp[] = {const_cast<char*>(xdg.c_str()), env_path.data(), nullptr};
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        pid_t pid = -1;
        ASSERT_EQ(posix_spawn(&pid, OSIE_CLI, &fa, nullptr, argv.data(), envp), 0);
        posix_spawn_file_actions_destroy(&fa);
        kids.push_back(pid);
    }
    for (pid_t p : kids) {
        int st = 0;
        ASSERT_EQ(::waitpid(p, &st, 0), p);
        EXPECT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    ProfileStore store(base + "/osie");
    auto all = store.load();
    ASSERT_TRUE(all.ok()) << all.error().describe();
    EXPECT_EQ(all->size(), static_cast<std::size_t>(kProcs)) << "the directory lock serialises writers";
    std::size_t entries = 0;
    for (const auto& e : std::filesystem::directory_iterator(store.dir())) {
        (void)e;
        ++entries;
    }
    EXPECT_EQ(entries, 1u) << "no temporary file left behind";
    std::filesystem::remove_all(base);
}

TEST(RemoteProfile, DefaultLocationFollowsXdg) {
    const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = old_xdg ? old_xdg : "";
    ::setenv("XDG_CONFIG_HOME", "/somewhere/cfg", 1);
    EXPECT_EQ(ProfileStore::for_user()->dir(), "/somewhere/cfg/osie");
    ::setenv("XDG_CONFIG_HOME", "relative/cfg", 1);  // ignored: not absolute
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') EXPECT_EQ(ProfileStore::for_user()->dir(), std::string(home) + "/.config/osie");
    if (old_xdg) ::setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    else ::unsetenv("XDG_CONFIG_HOME");
}
