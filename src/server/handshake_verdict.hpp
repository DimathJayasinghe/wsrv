// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace wsrv {

/// The application's answer to a waiting upgrade (ADR-0018 section 3).
struct HandshakeVerdict {
    bool accept = false;
    /// Accept only: the subprotocol to answer with. Empty keeps the one the server negotiated.
    std::optional<std::string> subprotocol;
    /// Reject only: one of kDecisionStatuses.
    int status = 403;
};

/// The statuses an application may refuse with. A fixed set, so no application text, header or
/// unexpected code ever reaches the response (header injection, information leaks).
inline constexpr std::array<int, 7> kDecisionStatuses{400, 401, 403, 404, 429, 500, 503};

[[nodiscard]] constexpr bool is_decision_status(int status) noexcept {
    return std::ranges::find(kDecisionStatuses, status) != kDecisionStatuses.end();
}

/// Seconds a refused client is told to wait (Retry-After) with 429 and 503 (ADR-0018 amendment 1).
inline constexpr int kRetryAfterSeconds = 5;

}  // namespace wsrv
