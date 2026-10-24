// SPDX-License-Identifier: Apache-2.0
#include "protocol/frame_writer.hpp"

#include <algorithm>

namespace wsrv {

namespace {
constexpr std::uint64_t kMaxPayload7Bit = 125;
constexpr std::uint64_t kMaxPayload16Bit = 65535;
constexpr std::uint64_t kTopBit = 0x8000000000000000ULL;
constexpr std::uint8_t kLength16Marker = 126;
constexpr std::uint8_t kLength64Marker = 127;
}  // namespace

std::size_t header_size(std::uint64_t payload_length, bool masked) noexcept {
    std::size_t size = 2;
    if (payload_length > kMaxPayload16Bit) {
        size += 8;
    } else if (payload_length > kMaxPayload7Bit) {
        size += 2;
    }
    return masked ? size + 4 : size;
}

namespace {

[[nodiscard]] std::uint8_t first_byte(const FrameHeader& header) noexcept {
    auto first = static_cast<unsigned>(header.opcode);
    first |= header.fin ? 0x80U : 0U;
    first |= header.rsv1 ? 0x40U : 0U;
    first |= header.rsv2 ? 0x20U : 0U;
    first |= header.rsv3 ? 0x10U : 0U;
    return static_cast<std::uint8_t>(first);
}

/// Write the 7-bit length field and, if needed, the extended length; return the bytes used (2, 4 or
/// 10).
[[nodiscard]] std::size_t write_length(MutableByteView out, const FrameHeader& header) noexcept {
    const unsigned mask_bit = header.masked ? 0x80U : 0U;
    if (header.payload_length <= kMaxPayload7Bit) {
        out[1] = static_cast<std::byte>(mask_bit | static_cast<unsigned>(header.payload_length));
        return 2;
    }
    if (header.payload_length <= kMaxPayload16Bit) {
        out[1] = static_cast<std::byte>(mask_bit | kLength16Marker);
        store_be16(out.subspan(2), static_cast<std::uint16_t>(header.payload_length));
        return 4;
    }
    out[1] = static_cast<std::byte>(mask_bit | kLength64Marker);
    store_be64(out.subspan(2), header.payload_length);
    return 10;
}

}  // namespace

Result<std::size_t> encode_header(MutableByteView out, const FrameHeader& header) {
    WSRV_ASSERT(out.size() >= kMaxHeaderSize);
    if ((header.payload_length & kTopBit) != 0) {
        return fail(ErrorCode::InvalidArgument, "payload length does not fit in 63 bits");
    }
    if (is_control(header.opcode) && (!header.fin || header.payload_length > kMaxControlPayload)) {
        return fail(ErrorCode::InvalidArgument,
                    "control frames must be final and at most 125 bytes");
    }
    out[0] = static_cast<std::byte>(first_byte(header));
    std::size_t size = write_length(out, header);
    if (header.masked) {
        std::copy(header.mask_key.begin(),
                  header.mask_key.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(size));
        size += header.mask_key.size();
    }
    return size;
}

}  // namespace wsrv
