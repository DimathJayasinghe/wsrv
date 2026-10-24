// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "protocol/failure.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wsrv {

/// A parsed Close payload. `reason` is a view into the payload that was parsed.
struct ClosePayload {
    std::uint16_t code;
    std::string_view reason;
};

/// Validate and parse the payload of a received Close frame (RFC 6455 section 5.5.1, 7.4):
///  - empty payload -> success with std::nullopt ("no status");
///  - exactly 1 byte, an invalid code, or a reason longer than 123 bytes -> 1002;
///  - a reason that is not valid UTF-8 -> 1007.
/// Pure, any-thread. The returned views point into `payload`.
[[nodiscard]] FrameResult<std::optional<ClosePayload>> parse_close_payload(ByteView payload);

/// Write a Close payload (big-endian code + reason) into `out` and return its size. The reason is
/// cut to at most 123 bytes at a UTF-8 code-point boundary, so the result always parses.
/// Preconditions (asserted): `code` is valid on the wire and out.size() >= kMaxControlPayload.
[[nodiscard]] std::size_t
encode_close_payload(MutableByteView out, CloseCode code, std::string_view reason);

}  // namespace wsrv
