// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token_format.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace wsrv::wsrvd {

namespace {

constexpr std::string_view kPrefix = "wsrv1_";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::size_t kChecksumDigits = 8;
constexpr std::size_t kBodySize = kPrefix.size() + (2 * kGeneratedTokenRandomBytes);

/// The reflected CRC-32 polynomial of IEEE 802.3.
constexpr std::uint32_t kPolynomial = 0xEDB88320U;

/// `value` as `out.size()` lowercase hex digits, most significant first.
void write_hex(std::span<char> out, std::uint64_t value) noexcept {
    for (std::size_t i = out.size(); i > 0; --i) {
        out[i - 1] = kHexDigits[value & 0xFU];
        value >>= 4U;
    }
}

[[nodiscard]] bool is_lower_hex(std::string_view text) noexcept {
    return std::ranges::all_of(text, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] std::uint32_t crc32_of(std::string_view text) noexcept {
    return crc32(as_bytes(text));
}

}  // namespace

std::uint32_t crc32(ByteView data) noexcept {
    // Bit by bit: this runs once per start on 79 bytes, so a table would be code for nothing.
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(b);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ kPolynomial : crc >> 1U;
        }
    }
    return ~crc;
}

std::string format_generated_token(ByteView random) {
    std::string token{kPrefix};
    for (const std::byte b : random) {
        std::array<char, 2> pair{};
        write_hex(pair, static_cast<std::uint64_t>(b));
        token.append(pair.data(), pair.size());
    }
    std::array<char, kChecksumDigits> checksum{};
    write_hex(checksum, crc32_of(token));
    token += '_';
    token.append(checksum.data(), checksum.size());
    return token;
}

bool is_generated_token(std::string_view token) noexcept {
    if (token.size() != kGeneratedTokenSize || !token.starts_with(kPrefix)) {
        return false;
    }
    // Pieces by pointer and length: the size is checked above, and substr() may throw.
    const std::string_view digits{token.data() + kPrefix.size(), 2 * kGeneratedTokenRandomBytes};
    const std::string_view body{token.data(), kBodySize};
    const std::string_view checksum{token.data() + kBodySize + 1, kChecksumDigits};
    if (token[kBodySize] != '_' || !is_lower_hex(digits)) {
        return false;
    }
    std::array<char, kChecksumDigits> expected{};
    write_hex(expected, crc32_of(body));
    return checksum == std::string_view{expected.data(), expected.size()};
}

}  // namespace wsrv::wsrvd
