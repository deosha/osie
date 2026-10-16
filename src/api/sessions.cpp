// SPDX-License-Identifier: Apache-2.0
#include "osie/api/sessions.hpp"

#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#if defined(__APPLE__)
#include <stdlib.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <stdexcept>

namespace osie::api {

SessionStore::SessionStore(std::size_t max_sessions, Clock::duration ttl)
    : max_(std::max<std::size_t>(1, max_sessions)), per_uid_(std::max<std::size_t>(1, max_ / 4)), ttl_(ttl) {
    if (max_ < 8) per_uid_ = max_;
}

bool SessionStore::valid_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
    });
}

std::string SessionStore::generate_id() {
    std::array<unsigned char, 16> b{};
#if defined(__APPLE__)
    arc4random_buf(b.data(), b.size());
#elif defined(__linux__)
    std::size_t got = 0;
    while (got < b.size()) {
        const auto n = ::getrandom(b.data() + got, b.size() - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("getrandom failed");
        }
        got += static_cast<std::size_t>(n);
    }
#else
#error "no system random source"
#endif
    static constexpr char kHex[] = "0123456789abcdef";
    std::string o;
    for (auto c : b) {
        o += kHex[c >> 4];
        o += kHex[c & 15];
    }
    return o;
}

void SessionStore::expire(Clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();)
        it = now - it->second.used > ttl_ ? sessions_.erase(it) : std::next(it);
}

std::shared_ptr<Session> SessionStore::get(uid_t uid, const std::string& id, Clock::time_point now) {
    std::lock_guard lk(mu_);
    expire(now);
    const auto key = std::make_pair(uid, id);
    if (auto it = sessions_.find(key); it != sessions_.end()) {
        it->second.used = now;
        return it->second.s;
    }
    // Make room: first within this uid's quota, then globally (least recently used).
    auto evict_lru = [&](auto pred) {
        auto victim = sessions_.end();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
            if (pred(it->first) && (victim == sessions_.end() || it->second.used < victim->second.used)) victim = it;
        if (victim != sessions_.end()) sessions_.erase(victim);
    };
    const auto own = static_cast<std::size_t>(std::count_if(sessions_.begin(), sessions_.end(),
                                                            [&](const auto& kv) { return kv.first.first == uid; }));
    if (own >= per_uid_) evict_lru([&](const auto& k) { return k.first == uid; });
    if (sessions_.size() >= max_) evict_lru([](const auto&) { return true; });
    auto s = std::make_shared<Session>();
    sessions_.emplace(key, Entry{s, now});
    return s;
}

std::size_t SessionStore::size() const {
    std::lock_guard lk(mu_);
    return sessions_.size();
}

}  // namespace osie::api
