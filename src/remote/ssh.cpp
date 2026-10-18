// SPDX-License-Identifier: Apache-2.0
#include "osie/remote/ssh.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

namespace osie::remote {
namespace {

bool executable_file(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

/// Characters that are safe in the local forwarding path: '%' and '$' would
/// be expanded by OpenSSH, ':' and '[' would change how -L is split.
bool safe_local_path(const std::string& s) {
    if (s.empty() || s.front() != '/' || s.find("/-") != std::string::npos) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
               c == '_' || c == '-';
    });
}

}  // namespace

const std::vector<std::string>& forced_ssh_options() {
    static const std::vector<std::string> kOptions = {
        // Never prompt: no password, passphrase or host-key questions.
        "BatchMode=yes",
        // Host keys: verified against known_hosts, never accepted or written.
        "StrictHostKeyChecking=yes",
        "UpdateHostKeys=no",
        "CheckHostIP=no",
        // The forward: fail instead of running without it; the listening
        // socket is created owner-only and never replaces an existing entry.
        "ExitOnForwardFailure=yes",
        "StreamLocalBindMask=0177",
        "StreamLocalBindUnlink=no",
        // Nothing else crosses the connection, and no session or command runs.
        "ForwardAgent=no",
        "ForwardX11=no",
        "ForwardX11Trusted=no",
        "Tunnel=no",
        "SessionType=none",
        "RemoteCommand=none",
        "RequestTTY=no",
        "StdinNull=yes",
        "EscapeChar=none",
        // Nothing runs locally, no shared or background connection.
        "PermitLocalCommand=no",
        "ControlMaster=no",
        "ControlPath=none",
        "ForkAfterAuthentication=no",
        "AddKeysToAgent=no",
        // Errors only, so stderr stays small and classifiable.
        "LogLevel=ERROR",
        // A dead connection is noticed; the overall deadline bounds the rest.
        "ServerAliveInterval=5",
        "ServerAliveCountMax=3",
    };
    return kOptions;
}

Result<std::vector<std::string>> build_ssh_argv(const std::string& ssh_path, const Profile& p,
                                                const std::string& local_socket, int connect_timeout_s) {
    if (auto e = check_profile(p)) return Error{Errc::invalid_argument, *e, 0};
    if (ssh_path.empty() || ssh_path.front() != '/') return Error{Errc::invalid_argument, "ssh path must be absolute", 0};
    if (!safe_local_path(local_socket) || local_socket.size() > 103)
        return Error{Errc::invalid_argument, "unsafe local forwarding path", 0};
    const int ct = std::clamp(connect_timeout_s, 1, kMaxTimeoutS);
    std::vector<std::string> a = {ssh_path, "-N", "-T"};
    for (const auto& o : forced_ssh_options()) {
        a.emplace_back("-o");
        a.push_back(o);
    }
    a.emplace_back("-o");
    a.push_back("ConnectTimeout=" + std::to_string(ct));
    if (!p.identity.empty()) {
        a.emplace_back("-o");
        a.push_back("IdentityFile=" + p.identity);
        a.emplace_back("-o");
        a.emplace_back("IdentitiesOnly=yes");
    }
    if (!p.known_hosts.empty()) {
        a.emplace_back("-o");
        a.push_back("UserKnownHostsFile=" + p.known_hosts);
    }
    if (p.port) {
        a.emplace_back("-p");
        a.push_back(std::to_string(*p.port));
    }
    a.emplace_back("-L");
    a.push_back(local_socket + ":" + p.socket);
    a.emplace_back("--");
    a.push_back(ssh_destination(p.destination));
    return a;
}

Result<std::string> locate_ssh() {
    const bool setid = ::getuid() != ::geteuid() || ::getgid() != ::getegid();
    if (!setid) {
        if (const char* o = std::getenv("OSIE_SSH"); o != nullptr && o[0] != '\0') {
            const std::string p(o);
            if (p.front() != '/') return Error{Errc::invalid_argument, "OSIE_SSH must be an absolute path", 0};
            if (!executable_file(p)) return Error{Errc::not_found, "OSIE_SSH is not an executable file", 0};
            return p;
        }
        if (const char* path = std::getenv("PATH"); path != nullptr) {
            std::string_view rest(path);
            while (true) {
                const auto colon = rest.find(':');
                const std::string dir(rest.substr(0, colon));
                if (!dir.empty() && dir.front() == '/') {
                    const std::string cand = (dir.back() == '/' ? dir : dir + "/") + "ssh";
                    if (executable_file(cand)) return cand;
                }
                if (colon == std::string_view::npos) break;
                rest.remove_prefix(colon + 1);
            }
        }
    }
    if (executable_file("/usr/bin/ssh")) return std::string("/usr/bin/ssh");
    return Error{Errc::not_found, "no OpenSSH client (ssh) found in PATH or /usr/bin", 0};
}

}  // namespace osie::remote
