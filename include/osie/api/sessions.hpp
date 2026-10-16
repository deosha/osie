// SPDX-License-Identifier: Apache-2.0
// Conversation sessions of the local API.
//
// A session is keyed by the peer's kernel-reported uid *and* its session id,
// so references such as "it" or "the second one" never cross users: a
// client that guesses another user's session id gets a fresh session of its
// own. Processes running as the same uid are one principal (they could
// read each other's memory anyway) and may share sessions by id. Ids the
// server generates are 128-bit random values.
//
// Bounded: at most `max_sessions` in total and `per_uid` for any one uid
// (so one user cannot evict everyone else's); idle sessions expire after
// `ttl`; the least recently used is evicted first.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "osie/engine/engine.hpp"

namespace osie::api {

struct Session {
    SessionContext context;  // touched only under the engine's serialisation
};

class SessionStore {
public:
    using Clock = std::chrono::steady_clock;
    SessionStore(std::size_t max_sessions, Clock::duration ttl);

    /// Valid ids: 1-64 characters of [A-Za-z0-9._-].
    static bool valid_id(std::string_view id) noexcept;
    /// A new random id (32 hex characters).
    static std::string generate_id();

    /// The session for (uid, id), created if absent. The returned pointer
    /// keeps the session alive even if it is evicted meanwhile.
    std::shared_ptr<Session> get(uid_t uid, const std::string& id, Clock::time_point now = Clock::now());

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t per_uid_limit() const noexcept { return per_uid_; }

private:
    void expire(Clock::time_point now);
    struct Entry {
        std::shared_ptr<Session> s;
        Clock::time_point used;
    };
    mutable std::mutex mu_;
    std::map<std::pair<uid_t, std::string>, Entry> sessions_;
    std::size_t max_;
    std::size_t per_uid_;
    Clock::duration ttl_;
};

}  // namespace osie::api
