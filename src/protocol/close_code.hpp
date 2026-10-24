// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol/opcode.hpp"

#include <cstddef>
#include <cstdint>

namespace wsrv {

/// Close status codes (RFC 6455 section 7.4, IANA registry). NoStatusReceived, AbnormalClosure and
/// TlsHandshake are for local reporting only and MUST NOT be sent on the wire (is_valid_on_wire is
/// false).
enum class CloseCode : std::uint16_t {
    Normal = 1000,
    GoingAway = 1001,
    ProtocolError = 1002,
    UnsupportedData = 1003,
    NoStatusReceived = 1005,
    AbnormalClosure = 1006,
    InvalidPayload = 1007,
    PolicyViolation = 1008,
    MessageTooBig = 1009,
    MandatoryExtension = 1010,
    InternalError = 1011,
    ServiceRestart = 1012,
    TryAgainLater = 1013,
    BadGateway = 1014,
    TlsHandshake = 1015,
};

/// May `code` appear in a Close frame? True for 1000-1003, 1007-1014 and the application range
/// 3000-4999; false for everything else (0-999, 1004-1006, 1015, 1016-2999 unassigned, >= 5000).
[[nodiscard]] constexpr bool is_valid_on_wire(std::uint16_t code) noexcept {
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) ||
           (code >= 3000 && code <= 4999);
}

/// The longest close reason that fits a control frame next to the 2-byte code.
inline constexpr std::size_t kMaxCloseReason = kMaxControlPayload - 2;

}  // namespace wsrv
