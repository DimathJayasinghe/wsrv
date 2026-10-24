// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol/close_code.hpp"

#include <expected>
#include <string_view>

namespace wsrv {

/// A protocol violation by the peer, expressed as the close code the server must answer with.
/// `reason` is a string literal (never peer data), suitable for logs and for the Close frame's
/// reason.
struct ProtocolFailure {
    CloseCode code;
    std::string_view reason;

    friend constexpr bool operator==(const ProtocolFailure&, const ProtocolFailure&) = default;
};

template <class T>
using FrameResult = std::expected<T, ProtocolFailure>;

[[nodiscard]] constexpr std::unexpected<ProtocolFailure>
protocol_failure(CloseCode code, std::string_view reason) noexcept {
    return std::unexpected<ProtocolFailure>{ProtocolFailure{code, reason}};
}

}  // namespace wsrv
