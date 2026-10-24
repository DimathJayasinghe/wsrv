// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol/masker.hpp"
#include "protocol/opcode.hpp"

#include <cstddef>
#include <cstdint>

namespace wsrv {

/// The decoded header of one WebSocket frame (RFC 6455 section 5.2). `mask_key` is meaningful only
/// if `masked`. `payload_length` is the declared length (at most 2^63 - 1).
struct FrameHeader {
    bool fin = true;
    bool rsv1 = false;
    bool rsv2 = false;
    bool rsv3 = false;
    Opcode opcode = Opcode::Text;
    bool masked = false;
    std::uint64_t payload_length = 0;
    MaskKey mask_key{};

    friend bool operator==(const FrameHeader&, const FrameHeader&) = default;
};

/// 2 bytes + up to 8 bytes of extended length + 4 bytes of mask key.
inline constexpr std::size_t kMaxHeaderSize = 14;

}  // namespace wsrv
