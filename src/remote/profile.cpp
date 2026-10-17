// SPDX-License-Identifier: Apache-2.0
#include "osie/remote/profile.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>

#include "osie/util/secure_path.hpp"

namespace osie::remote {
namespace {

bool is_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::optional<std::string> check_user(std::string_view u) {
    if (u.empty() || u.size() > 32) return "the user name must be 1-32 characters";
    if (!(is_alnum(u[0]) || u[0] == '_') || is_digit(u[0]))
        return "the user name must start with a letter or '_'";
    for (char c : u)
        if (!(is_alnum(c) || c == '_' || c == '.' || c == '-')) return "the user name may contain only [A-Za-z0-9._-]";
    return std::nullopt;
}

std::optional<std::string> check_host(std::string_view h) {
    if (h.empty()) return "the host is empty";
    if (h.front() == '[') {
        if (h.size() < 4 || h.back() != ']') return "an IPv6 address must be written as [address]";
        const std::string inner(h.substr(1, h.size() - 2));
        if (inner.size() > 45) return "the IPv6 address is too long";
        for (char c : inner)
            if (!(is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.'))
                return "the IPv6 address may contain only hexadecimal digits, ':' and '.' (no zone index)";
        in6_addr a{};
        if (::inet_pton(AF_INET6, inner.c_str(), &a) != 1) return "the IPv6 address is not valid";
        return std::nullopt;
    }
    if (h.size() > 253) return "the host name is longer than 253 characters";
    bool all_numeric = true;
    std::size_t i = 0;
    while (i <= h.size()) {
        const std::size_t j = std::min(h.find('.', i), h.size());
        const std::string_view label = h.substr(i, j - i);
        if (label.empty()) return "the host name has an empty label";
        if (label.size() > 63) return "a host name label is longer than 63 characters";
        if (label.front() == '-' || label.back() == '-') return "a host name label may not start or end with '-'";
        for (char c : label) {
            if (!(is_alnum(c) || c == '-' || c == '_')) return "the host may contain only [A-Za-z0-9._-] (or [IPv6])";
            if (!is_digit(c)) all_numeric = false;
        }
        i = j + 1;
    }
    if (all_numeric) {
        in_addr a{};
        if (::inet_pton(AF_INET, std::string(h).c_str(), &a) != 1) return "the IPv4 address is not valid";
    }
    return std::nullopt;
}

/// Shared by socket and file paths.
std::optional<std::string> check_abs_path(std::string_view s, std::size_t max, std::string_view extra, const char* what) {
    if (s.empty()) return std::string(what) + " is empty";
    if (s.front() != '/') return std::string(what) + " must be an absolute path";
    if (s.size() > max) return std::string(what) + " is longer than " + std::to_string(max) + " bytes";
    for (char c : s)
        if (!(is_alnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || extra.find(c) != std::string_view::npos))
            return std::string(what) + " may contain only [A-Za-z0-9/._" + std::string(extra) + "-]";
    if (s.size() == 1) return std::string(what) + " must name a file, not /";
    std::size_t i = 1;
    while (i <= s.size()) {
        const std::size_t j = std::min(s.find('/', i), s.size());
        const std::string_view comp = s.substr(i, j - i);
        if (comp.empty()) return std::string(what) + " has an empty component (\"//\" or a trailing '/')";
        if (comp == "." || comp == "..") return std::string(what) + " may not contain \".\" or \"..\" components";
        if (comp.front() == '-') return std::string(what) + " has a component starting with '-'";
        i = j + 1;
    }
    return std::nullopt;
}

std::string trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return std::string(s);
}

Error bad(std::string m) { return Error{Errc::invalid_argument, std::move(m), 0}; }

/// Reads remotes.conf from an open, validated directory.
Result<std::vector<Profile>> load_at(int dirfd, const std::string& file) {
    secure::Fd f(::openat(dirfd, "remotes.conf", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
    if (!f) {
        if (errno == ENOENT) return std::vector<Profile>{};
        if (errno == ELOOP) return Error{Errc::permission_denied, file + " is a symbolic link; refusing to use it", ELOOP};
        return Error{Errc::io_error, "cannot open " + file + ": " + std::strerror(errno), errno};
    }
    struct stat st {};
    if (::fstat(f.get(), &st) != 0) return Error{Errc::io_error, "cannot stat " + file, errno};
    if (!S_ISREG(st.st_mode)) return Error{Errc::permission_denied, file + " is not a regular file; refusing to use it", 0};
    if (st.st_uid != ::geteuid())
        return Error{Errc::permission_denied, file + " is owned by uid " + std::to_string(st.st_uid) + "; refusing to use it", 0};
    if ((st.st_mode & 077) != 0)
        return Error{Errc::permission_denied, file + " must have mode 0600 (chmod 600); refusing to use it", 0};
    if (st.st_size > static_cast<off_t>(kMaxProfileFileBytes))
        return Error{Errc::invalid_argument, file + " is larger than " + std::to_string(kMaxProfileFileBytes) + " bytes", 0};
    std::string text;
    char buf[8192];
    while (true) {
        const auto n = ::read(f.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Error{Errc::io_error, "cannot read " + file, errno};
        }
        if (n == 0) break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() > kMaxProfileFileBytes)
            return Error{Errc::invalid_argument, file + " is larger than " + std::to_string(kMaxProfileFileBytes) + " bytes", 0};
    }
    auto r = parse_profiles(text);
    if (!r) return Error{r.error().code, file + ": " + r.error().message, 0};
    return r;
}

Result<void> check_config_dir(int fd, const std::string& dir) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) return Error{Errc::io_error, "cannot stat " + dir, errno};
    if (st.st_uid != ::geteuid())
        return Error{Errc::permission_denied, dir + " is owned by uid " + std::to_string(st.st_uid) + "; refusing to use it", 0};
    if ((st.st_mode & 077) != 0)
        return Error{Errc::permission_denied, dir + " must have mode 0700 (chmod 700); refusing to use it", 0};
    return {};
}

/// Creates the missing trailing components of an absolute `dir`, each 0700.
Result<void> make_dirs(const std::string& dir) {
    std::vector<std::string> missing;
    std::string p = dir;
    while (p.size() > 1) {
        struct stat st {};
        if (::lstat(p.c_str(), &st) == 0) break;
        if (errno != ENOENT) return Error{Errc::io_error, "cannot stat " + p + ": " + std::strerror(errno), errno};
        missing.push_back(p);
        const auto slash = p.rfind('/');
        p = slash == 0 ? "/" : p.substr(0, slash);
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it)
        if (::mkdir(it->c_str(), 0700) != 0 && errno != EEXIST)
            return Error{Errc::io_error, "cannot create " + *it + ": " + std::strerror(errno), errno};
    return {};
}

}  // namespace

std::optional<long> parse_decimal(std::string_view s) {
    if (s.empty() || s.size() > 9) return std::nullopt;
    long v = 0;
    for (char c : s) {
        if (!is_digit(c)) return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

std::optional<std::string> check_name(std::string_view s) {
    if (s.empty() || s.size() > 64) return "the name must be 1-64 characters";
    if (!is_alnum(s.front())) return "the name must start with a letter or digit";
    for (char c : s)
        if (!(is_alnum(c) || c == '.' || c == '_' || c == '-')) return "the name may contain only [A-Za-z0-9._-]";
    return std::nullopt;
}

std::optional<std::string> check_destination(std::string_view s) {
    if (s.empty()) return "the destination is empty";
    if (s.size() > 300) return "the destination is too long";
    if (s.front() == '-') return "the destination may not start with '-'";
    const auto at = s.find('@');
    if (at != std::string_view::npos) {
        if (s.find('@', at + 1) != std::string_view::npos) return "the destination may contain at most one '@'";
        if (auto e = check_user(s.substr(0, at))) return e;
        return check_host(s.substr(at + 1));
    }
    return check_host(s);
}

std::optional<std::string> check_port(long v) {
    if (v < 1 || v > 65535) return "the port must be from 1 to 65535";
    return std::nullopt;
}

std::optional<std::string> check_timeout(long v) {
    if (v < kMinTimeoutS || v > kMaxTimeoutS)
        return "the timeout must be from " + std::to_string(kMinTimeoutS) + " to " + std::to_string(kMaxTimeoutS) + " seconds";
    return std::nullopt;
}

std::optional<std::string> check_socket_path(std::string_view s) { return check_abs_path(s, 100, "", "the socket path"); }

std::optional<std::string> check_file_path(std::string_view s) { return check_abs_path(s, 1024, "+@", "the path"); }

std::optional<std::string> check_profile(const Profile& p) {
    if (auto e = check_name(p.name)) return e;
    if (auto e = check_destination(p.destination)) return e;
    if (p.port)
        if (auto e = check_port(*p.port)) return e;
    if (auto e = check_socket_path(p.socket)) return e;
    if (!p.identity.empty())
        if (auto e = check_file_path(p.identity)) return "identity: " + *e;
    if (!p.known_hosts.empty())
        if (auto e = check_file_path(p.known_hosts)) return "known_hosts: " + *e;
    if (auto e = check_timeout(p.timeout_s)) return e;
    return std::nullopt;
}

std::string ssh_destination(std::string_view d) {
    const auto at = d.find('@');
    const std::string_view user = at == std::string_view::npos ? std::string_view{} : d.substr(0, at + 1);
    std::string_view host = at == std::string_view::npos ? d : d.substr(at + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    return std::string(user) + std::string(host);
}

Result<std::vector<Profile>> parse_profiles(std::string_view text) {
    if (text.size() > kMaxProfileFileBytes) return bad("the profile file is too large");
    std::vector<Profile> out;
    std::set<std::string> names, keys;
    Profile* cur = nullptr;
    std::size_t lineno = 0, i = 0;
    auto finish = [&]() -> std::optional<std::string> {
        if (cur == nullptr) return std::nullopt;
        if (cur->destination.empty()) return "profile [" + cur->name + "] has no destination";
        if (auto e = check_profile(*cur)) return "profile [" + cur->name + "]: " + *e;
        return std::nullopt;
    };
    while (i < text.size()) {
        ++lineno;
        const std::size_t j = std::min(text.find('\n', i), text.size());
        const std::string_view line = text.substr(i, j - i);
        i = j + 1;
        const std::string where = "line " + std::to_string(lineno) + ": ";
        if (line.size() > 1100) return bad(where + "too long");
        for (unsigned char c : line)
            if (c < 0x20 || c >= 0x7f) return bad(where + "control or non-ASCII character");
        const std::string t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        if (t.front() == '[') {
            if (t.back() != ']') return bad(where + "malformed section header");
            if (auto e = finish()) return bad(*e);
            const std::string name = t.substr(1, t.size() - 2);
            if (auto e = check_name(name)) return bad(where + *e);
            if (!names.insert(name).second) return bad(where + "duplicate profile [" + name + "]");
            if (out.size() >= kMaxProfiles) return bad(where + "more than " + std::to_string(kMaxProfiles) + " profiles");
            out.emplace_back();
            cur = &out.back();
            cur->name = name;
            keys.clear();
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) return bad(where + "expected key = value");
        const std::string key = trim(std::string_view(t).substr(0, eq));
        const std::string val = trim(std::string_view(t).substr(eq + 1));
        if (cur == nullptr) return bad(where + "key outside a [profile] section");
        if (!keys.insert(key).second) return bad(where + "duplicate key " + key);
        std::optional<std::string> e;
        if (key == "destination") {
            e = check_destination(val);
            cur->destination = val;
        } else if (key == "port") {
            const auto v = parse_decimal(val);
            e = v ? check_port(*v) : std::optional<std::string>("the port must be a number");
            if (v) cur->port = static_cast<int>(*v);
        } else if (key == "socket") {
            e = check_socket_path(val);
            cur->socket = val;
        } else if (key == "identity") {
            e = check_file_path(val);
            cur->identity = val;
        } else if (key == "known_hosts") {
            e = check_file_path(val);
            cur->known_hosts = val;
        } else if (key == "timeout") {
            const auto v = parse_decimal(val);
            e = v ? check_timeout(*v) : std::optional<std::string>("the timeout must be a number of seconds");
            if (v) cur->timeout_s = static_cast<int>(*v);
        } else {
            return bad(where + "unknown key");
        }
        if (e) return bad(where + key + ": " + *e);
    }
    if (auto e = finish()) return bad(*e);
    return out;
}

std::string serialize_profiles(const std::vector<Profile>& ps) {
    std::string o = "# osie remote profiles. Managed by `osie remote add/remove`; see docs/REMOTE.md.\n"
                    "# No secrets belong here: keys and agents stay OpenSSH's business.\n";
    for (const auto& p : ps) {
        o += "\n[" + p.name + "]\n";
        o += "destination = " + p.destination + "\n";
        if (p.port) o += "port = " + std::to_string(*p.port) + "\n";
        o += "socket = " + p.socket + "\n";
        if (!p.identity.empty()) o += "identity = " + p.identity + "\n";
        if (!p.known_hosts.empty()) o += "known_hosts = " + p.known_hosts + "\n";
        o += "timeout = " + std::to_string(p.timeout_s) + "\n";
    }
    return o;
}

Result<ProfileStore> ProfileStore::for_user() {
    // Environment variables are the invoking user's own; a set-id context
    // (osie is never installed set-id) would use the password database only.
    const bool setid = ::getuid() != ::geteuid() || ::getgid() != ::getegid();
    if (!setid) {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg != nullptr && xdg[0] == '/') return ProfileStore(std::string(xdg) + "/osie");
        const char* home = std::getenv("HOME");
        if (home != nullptr && home[0] == '/') return ProfileStore(std::string(home) + "/.config/osie");
    }
    const passwd* pw = ::getpwuid(::geteuid());
    if (pw != nullptr && pw->pw_dir != nullptr && pw->pw_dir[0] == '/')
        return ProfileStore(std::string(pw->pw_dir) + "/.config/osie");
    return Error{Errc::not_found, "no configuration directory: set HOME or XDG_CONFIG_HOME to an absolute path", 0};
}

Result<std::vector<Profile>> ProfileStore::load() const {
    secure::OpenOptions oo;
    oo.what = "remote profiles";
    oo.exclusive_final_dir = true;
    auto d = secure::open_trusted_dir(dir_, oo);
    if (!d) {
        if (d.error().code == Errc::not_found) return std::vector<Profile>{};
        return d.error();
    }
    if (auto r = check_config_dir(d->get(), dir_); !r) return r.error();
    return load_at(d->get(), file());
}

Result<void> ProfileStore::update(const std::function<Result<void>(std::vector<Profile>&)>& edit) const {
    if (dir_.empty() || dir_.front() != '/') return bad("the configuration directory must be absolute");
    if (auto r = make_dirs(dir_); !r) return r;
    secure::OpenOptions oo;
    oo.what = "remote profiles";
    oo.exclusive_final_dir = true;
    auto d = secure::open_trusted_dir(dir_, oo);
    if (!d) return d.error();
    const int dfd = d->get();
    if (auto r = check_config_dir(dfd, dir_); !r) return r;
    while (::flock(dfd, LOCK_EX) != 0)
        if (errno != EINTR) return Error{Errc::io_error, "cannot lock " + dir_, errno};
    auto cur = load_at(dfd, file());
    if (!cur) return cur.error();
    std::vector<Profile> ps = std::move(cur).value();
    if (auto r = edit(ps); !r) return r;
    for (const auto& p : ps)
        if (auto e = check_profile(p)) return bad("profile [" + p.name + "]: " + *e);
    const std::string text = serialize_profiles(ps);
    if (!parse_profiles(text)) return Error{Errc::internal, "serialised profiles do not parse", 0};

    unsigned char rnd[8];
    arc4random_buf(rnd, sizeof rnd);
    std::string tmp = ".remotes.conf.";
    static const char* kHex = "0123456789abcdef";
    for (unsigned char b : rnd) {
        tmp += kHex[b >> 4];
        tmp += kHex[b & 15];
    }
    secure::Fd f(::openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!f) return Error{Errc::io_error, "cannot create a temporary file in " + dir_ + ": " + std::strerror(errno), errno};
    auto fail = [&](const std::string& what) -> Result<void> {
        const int err = errno;
        ::unlinkat(dfd, tmp.c_str(), 0);
        return Error{Errc::io_error, what + " " + dir_ + ": " + std::strerror(err), err};
    };
    if (::fchmod(f.get(), 0600) != 0) return fail("cannot set the mode of a file in");
    std::size_t off = 0;
    while (off < text.size()) {
        const auto n = ::write(f.get(), text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail("cannot write to");
        off += static_cast<std::size_t>(n);
    }
    if (::fsync(f.get()) != 0) return fail("cannot sync a file in");
    f.reset();
    if (::renameat(dfd, tmp.c_str(), dfd, "remotes.conf") != 0) return fail("cannot replace remotes.conf in");
    (void)::fsync(dfd);
    return {};
}

}  // namespace osie::remote
