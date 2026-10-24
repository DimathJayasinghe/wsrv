// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace wsrv {

/// Read-only view of binary data. Binary data is always std::byte; text is std::string_view.
using ByteView = std::span<const std::byte>;
/// Writable view of binary data.
using MutableByteView = std::span<std::byte>;

/// View the characters of `text` as bytes (no copy; same lifetime as `text`).
[[nodiscard]] inline ByteView as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

/// View `bytes` as characters (no copy, no validation). The one audited reinterpret_cast in the
/// code base: std::byte and char alias any object (no strict-aliasing issue) and have equal size.
[[nodiscard]] inline std::string_view as_chars(ByteView bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): audited, see comment above.
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// Read a big-endian (network order) integer from the first N bytes. Precondition: size >= N.
/// Assembled byte by byte: no unaligned access, no endianness assumption, no aliasing.
[[nodiscard]] inline std::uint16_t load_be16(ByteView bytes) noexcept {
    WSRV_ASSERT(bytes.size() >= 2);
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(bytes[0]) << 8U) |
                                      std::to_integer<unsigned>(bytes[1]));
}

[[nodiscard]] inline std::uint32_t load_be32(ByteView bytes) noexcept {
    WSRV_ASSERT(bytes.size() >= 4);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(bytes[i]);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t load_be64(ByteView bytes) noexcept {
    WSRV_ASSERT(bytes.size() >= 8);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[i]);
    }
    return value;
}

/// Write `value` big-endian into the first N bytes of `out`. Precondition: size >= N.
inline void store_be16(MutableByteView out, std::uint16_t value) noexcept {
    WSRV_ASSERT(out.size() >= 2);
    out[0] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[1] = static_cast<std::byte>(value & 0xFFU);
}

inline void store_be32(MutableByteView out, std::uint32_t value) noexcept {
    WSRV_ASSERT(out.size() >= 4);
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((value >> (8U * (3U - i))) & 0xFFU);
    }
}

inline void store_be64(MutableByteView out, std::uint64_t value) noexcept {
    WSRV_ASSERT(out.size() >= 8);
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((value >> (8U * (7U - i))) & 0xFFU);
    }
}

}  // namespace wsrv
