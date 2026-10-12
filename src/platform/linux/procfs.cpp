// SPDX-License-Identifier: Apache-2.0
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>

#include "linux_os.hpp"

namespace osie::linux_os {

namespace {
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

std::optional<std::uint64_t> to_u64(std::string_view s) {
    std::uint64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p == s.data()) return std::nullopt;
    return v;
}
}  // namespace

Result<std::string> read_file(const std::string& path, std::size_t limit) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return make_error(errc_from_errno(errno), path, errno);
    std::string out;
    char buf[4096];
    while (out.size() < limit) {
        const ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return make_error(errc_from_errno(errno), path, errno);
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

std::optional<std::uint64_t> read_u64(const std::string& path) {
    auto r = read_file(path, 64);
    if (!r) return std::nullopt;
    return to_u64(trim(*r));
}

std::optional<std::uint64_t> kv_field(std::string_view text, std::string_view key, bool kb) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto eol = text.find('\n', pos);
        const auto line = text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? text.size() : eol + 1;
        if (line.size() <= key.size() || line.substr(0, key.size()) != key || line[key.size()] != ':') continue;
        auto rest = trim(line.substr(key.size() + 1));
        const auto sp = rest.find_first_of(" \t");
        auto v = to_u64(rest.substr(0, sp));
        if (!v) return std::nullopt;
        return kb ? *v * 1024 : *v;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> space_field(std::string_view text, std::string_view key) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto eol = text.find('\n', pos);
        const auto line = text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? text.size() : eol + 1;
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == ' ')
            return to_u64(trim(line.substr(key.size() + 1)));
    }
    return std::nullopt;
}

std::vector<unsigned> parse_cpu_list(std::string_view s) {
    std::vector<unsigned> v;
    s = trim(s);
    std::size_t pos = 0;
    while (pos < s.size()) {
        auto comma = s.find(',', pos);
        auto part = s.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        pos = comma == std::string_view::npos ? s.size() : comma + 1;
        const auto dash = part.find('-');
        auto a = to_u64(part.substr(0, dash));
        if (!a) continue;
        auto b = dash == std::string_view::npos ? a : to_u64(part.substr(dash + 1));
        if (!b || *b < *a || *b - *a > 4096) continue;
        for (auto i = *a; i <= *b; ++i) v.push_back(static_cast<unsigned>(i));
    }
    return v;
}

std::optional<PressureStall> parse_psi(std::string_view body, PsiResource r) {
    PressureStall ps;
    ps.resource = r;
    bool some = false;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto eol = body.find('\n', pos);
        auto line = body.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? body.size() : eol + 1;
        PsiLine pl;
        bool ok = false;
        auto field = [&](std::string_view k) -> std::optional<std::string_view> {
            const auto i = line.find(k);
            if (i == std::string_view::npos) return std::nullopt;
            auto rest = line.substr(i + k.size());
            return rest.substr(0, rest.find(' '));
        };
        if (auto a = field("avg10=")) { pl.avg10 = std::strtod(std::string(*a).c_str(), nullptr); ok = true; }
        if (auto a = field("avg60=")) pl.avg60 = std::strtod(std::string(*a).c_str(), nullptr);
        if (auto a = field("avg300=")) pl.avg300 = std::strtod(std::string(*a).c_str(), nullptr);
        if (auto a = field("total=")) pl.total_us = to_u64(*a).value_or(0);
        if (!ok) continue;
        if (line.rfind("some", 0) == 0) {
            ps.some = pl;
            some = true;
        } else if (line.rfind("full", 0) == 0) {
            ps.full = pl;
        }
    }
    if (!some) return std::nullopt;
    return ps;
}

std::optional<ProcStat> parse_proc_stat(std::string_view s) {
    // Format: pid (comm) state ppid ... ; comm may contain spaces and ')'.
    const auto lp = s.find('(');
    const auto rp = s.rfind(')');
    if (lp == std::string_view::npos || rp == std::string_view::npos || rp < lp) return std::nullopt;
    ProcStat st;
    st.comm = std::string(s.substr(lp + 1, rp - lp - 1));
    std::vector<std::string_view> f;  // fields starting at "state" (field 3)
    std::size_t pos = rp + 2;
    while (pos < s.size()) {
        auto sp = s.find(' ', pos);
        if (sp == std::string_view::npos) sp = s.size();
        f.push_back(s.substr(pos, sp - pos));
        pos = sp + 1;
    }
    if (f.size() < 22) return std::nullopt;
    auto u = [&](std::size_t field_no) { return to_u64(trim(f[field_no - 3])).value_or(0); };
    st.state = f[0].empty() ? '?' : f[0][0];
    st.ppid = static_cast<Pid>(u(4));
    st.majflt = u(12);
    st.utime_ticks = u(14);
    st.stime_ticks = u(15);
    st.nice = static_cast<std::int32_t>(std::strtol(std::string(f[19 - 3]).c_str(), nullptr, 10));
    st.threads = static_cast<std::uint32_t>(u(20));
    st.start_ticks = u(22);
    st.vsize = u(23);
    st.rss_pages = u(24);
    return st;
}

}  // namespace osie::linux_os
