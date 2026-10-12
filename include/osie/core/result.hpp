// SPDX-License-Identifier: Apache-2.0
// Result<T>: explicit, allocation-light error handling used by every layer.
// C++20 has no std::expected; this is a deliberately small subset of it.
#pragma once

#include <cassert>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace osie {

enum class Errc : unsigned char {
    permission_denied,  // OS refused (EPERM/EACCES, SIP, TCC, not root)
    not_found,          // object does not exist (e.g. no such PID)
    process_exited,     // process vanished during inspection (race)
    unsupported,        // feature does not exist on this platform/kernel
    unavailable,        // feature exists but no data right now
    invalid_argument,   // caller error, failed validation
    parse_error,        // malformed kernel/proc output or model file
    io_error,           // read/open failure not covered above
    cancelled,          // cooperative cancellation requested
    stale,              // observation too old for the requested use
    policy_denied,      // safety policy refused an operation
    internal,           // bug / invariant violation
};

constexpr std::string_view to_string(Errc e) noexcept {
    switch (e) {
        case Errc::permission_denied: return "permission_denied";
        case Errc::not_found: return "not_found";
        case Errc::process_exited: return "process_exited";
        case Errc::unsupported: return "unsupported";
        case Errc::unavailable: return "unavailable";
        case Errc::invalid_argument: return "invalid_argument";
        case Errc::parse_error: return "parse_error";
        case Errc::io_error: return "io_error";
        case Errc::cancelled: return "cancelled";
        case Errc::stale: return "stale";
        case Errc::policy_denied: return "policy_denied";
        case Errc::internal: return "internal";
    }
    return "unknown";
}

struct Error {
    Errc code{Errc::internal};
    std::string message;
    int sys_errno{0};

    [[nodiscard]] std::string describe() const {
        std::string s{to_string(code)};
        if (!message.empty()) {
            s += ": ";
            s += message;
        }
        if (sys_errno != 0) {
            s += " (errno ";
            s += std::to_string(sys_errno);
            s += ")";
        }
        return s;
    }
};

/// Maps an errno value to the closest Errc.
Errc errc_from_errno(int err) noexcept;

inline Error make_error(Errc c, std::string msg = {}, int err = 0) {
    return Error{c, std::move(msg), err};
}

template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> is ambiguous");

public:
    Result(T v) : v_(std::in_place_index<0>, std::move(v)) {}          // NOLINT(google-explicit-constructor)
    Result(Error e) : v_(std::in_place_index<1>, std::move(e)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { assert(ok()); return std::get<0>(v_); }
    const T& value() const& { assert(ok()); return std::get<0>(v_); }
    T&& value() && { assert(ok()); return std::get<0>(std::move(v_)); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    const Error& error() const& { assert(!ok()); return std::get<1>(v_); }

    template <class U>
    T value_or(U&& fallback) const& {
        return ok() ? std::get<0>(v_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error e) : err_(std::move(e)), has_err_(true) {}  // NOLINT(google-explicit-constructor)
    [[nodiscard]] bool ok() const noexcept { return !has_err_; }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const { assert(has_err_); return err_; }

private:
    Error err_{};
    bool has_err_{false};
};

}  // namespace osie
