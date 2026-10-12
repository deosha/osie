// SPDX-License-Identifier: Apache-2.0
// Every value OSIE reports carries provenance: which native interface
// produced it, when, and how trustworthy it is. Observed values are never
// mixed with predictions; predictions use a separate type (see reasoning/).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace osie {

using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;
using Millis = std::chrono::milliseconds;

enum class Platform : std::uint8_t { macos = 1U << 0U, linux_os = 1U << 1U };

constexpr std::string_view to_string(Platform p) noexcept {
    return p == Platform::macos ? "macos" : "linux";
}

/// Bit set of platforms. Used for capability and knowledge-graph metadata.
class PlatformSet {
public:
    constexpr PlatformSet() = default;
    constexpr PlatformSet(std::initializer_list<Platform> ps) {
        for (auto p : ps) bits_ |= static_cast<std::uint8_t>(p);
    }
    static constexpr PlatformSet all() { return {Platform::macos, Platform::linux_os}; }
    [[nodiscard]] constexpr bool contains(Platform p) const noexcept {
        return (bits_ & static_cast<std::uint8_t>(p)) != 0;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] std::string to_string() const;
    constexpr bool operator==(const PlatformSet&) const = default;

private:
    std::uint8_t bits_{0};
};

Platform host_platform() noexcept;

enum class Validity : std::uint8_t {
    valid,        // complete, direct observation
    partial,      // some fields could not be read (permission, race)
    stale,        // older than the freshness budget of the consumer
    unavailable,  // no value; the field must not be interpreted
    derived,      // computed from >=2 valid observations (rates, deltas)
};

constexpr std::string_view to_string(Validity v) noexcept {
    switch (v) {
        case Validity::valid: return "valid";
        case Validity::partial: return "partial";
        case Validity::stale: return "stale";
        case Validity::unavailable: return "unavailable";
        case Validity::derived: return "derived";
    }
    return "?";
}

struct Provenance {
    /// Native interface identifier, e.g. "mach.host_processor_info" or
    /// "procfs:/proc/stat". Always a string literal (static storage).
    std::string_view source{};
    WallTime wall{};
    SteadyTime mono{};
    Validity validity{Validity::unavailable};
    std::string note{};  // optional human-readable caveat

    static Provenance now(std::string_view src, Validity v = Validity::valid) {
        return Provenance{src, std::chrono::system_clock::now(), std::chrono::steady_clock::now(), v, {}};
    }
    [[nodiscard]] Millis age(SteadyTime at = std::chrono::steady_clock::now()) const {
        return std::chrono::duration_cast<Millis>(at - mono);
    }
    [[nodiscard]] bool fresh(Millis budget, SteadyTime at = std::chrono::steady_clock::now()) const {
        return validity != Validity::unavailable && age(at) <= budget;
    }
};

template <class T>
struct Observation {
    T value{};
    Provenance prov{};
};

/// Cooperative cancellation (std::stop_token is not available in Apple libc++
/// without experimental flags, so OSIE uses this minimal equivalent).
class CancellationSource;
class CancellationToken {
public:
    CancellationToken() = default;
    [[nodiscard]] bool cancelled() const noexcept {
        return flag_ && flag_->load(std::memory_order_relaxed);
    }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> f) : flag_(std::move(f)) {}
    std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] CancellationToken token() const { return CancellationToken{flag_}; }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace osie
