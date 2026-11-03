// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wsrv {

/// Readiness bits, used both as interest (Read, Write) and as reported events (all four).
enum class PollEvents : std::uint8_t {
    None = 0,
    Read = 1,    ///< data (or EOF) can be read without blocking
    Write = 2,   ///< data can be written without blocking
    Error = 4,   ///< the descriptor is in an error state (read it to learn which)
    Hangup = 8,  ///< the peer closed or reset; reading yields the rest of the data, then EOF
};

// The analyzer sees PollEvents as a plain enum; combinations of the bits are intended (bit flags).
[[nodiscard]] constexpr PollEvents operator|(PollEvents a, PollEvents b) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): bit-flag combination
    return static_cast<PollEvents>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr PollEvents operator&(PollEvents a, PollEvents b) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): bit-flag combination
    return static_cast<PollEvents>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
/// True if `set` contains every bit of `bits` (and `bits` is not empty).
[[nodiscard]] constexpr bool has(PollEvents set, PollEvents bits) noexcept {
    return bits != PollEvents::None && (set & bits) == bits;
}

/// One ready descriptor. `token` is whatever the registrant chose; the event loop packs a handle
/// plus a generation into it, so a stale event can be recognised (EC-N12).
struct PollEvent {
    std::uint64_t token = 0;
    PollEvents events = PollEvents::None;
};

/// Readiness-based I/O multiplexing (ADR-0004): "which of these descriptors can I read or write
/// now". Registrations are EDGE-TRIGGERED: an event is reported when readiness appears, not
/// repeatedly while it persists, so the consumer must read / write until WouldBlock (EC-N07).
///
/// Thread-affinity: loop-thread-only.
class IPoller {
public:
    IPoller() = default;
    IPoller(const IPoller&) = delete;
    IPoller& operator=(const IPoller&) = delete;
    IPoller(IPoller&&) = delete;
    IPoller& operator=(IPoller&&) = delete;
    virtual ~IPoller() = default;

    /// Start watching `fd` for `interest` (Read and/or Write; Error is always reported, and so
    /// is a full hang-up (HUP). Asking for Hangup also reports a peer that merely closed its
    /// sending side, which Read implies). Error if `fd` is already registered or invalid.
    [[nodiscard]] virtual Result<void> add(int fd, PollEvents interest, std::uint64_t token) = 0;
    /// Replace the interest and token of a registered descriptor. Also re-arms the edge: if the
    /// descriptor is ready now, it is reported again.
    [[nodiscard]] virtual Result<void> modify(int fd, PollEvents interest, std::uint64_t token) = 0;
    /// Stop watching `fd`. Error if it is not registered.
    [[nodiscard]] virtual Result<void> remove(int fd) = 0;

    /// Wait up to `timeout_ms` (0 = just look, negative = until something happens) and fill `out`
    /// with ready descriptors. @return how many were written; readiness that does not fit stays
    /// pending for the next call. An interrupted wait (EINTR) returns 0, not an error.
    [[nodiscard]] virtual Result<std::size_t> wait(int timeout_ms, std::span<PollEvent> out) = 0;
};

}  // namespace wsrv
