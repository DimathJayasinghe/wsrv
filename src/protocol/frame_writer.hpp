// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "protocol/frame.hpp"

#include <cstddef>
#include <cstdint>

namespace wsrv {

/// Size in bytes of the encoded header for a payload of `payload_length` bytes (shortest length
/// form).
[[nodiscard]] std::size_t header_size(std::uint64_t payload_length, bool masked) noexcept;

/// Encode `header` into `out` using the SHORTEST legal length encoding and return the number of
/// bytes written (see header_size). The caller appends the payload (already masked, if
/// `header.masked`). RSV bits and the mask key are written as given. Precondition (asserted):
/// out.size() >= kMaxHeaderSize.
/// @return Error{InvalidArgument} if the frame could not be legal: a control frame that is
/// fragmented or has more than 125 payload bytes, or a payload length >= 2^63.
[[nodiscard]] Result<std::size_t> encode_header(MutableByteView out, const FrameHeader& header);

}  // namespace wsrv
