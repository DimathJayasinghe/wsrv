// SPDX-License-Identifier: Apache-2.0
#include "crypto/base64.hpp"

#include <array>
#include <cstdint>

namespace wsrv {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kPad = '=';
constexpr std::int8_t kInvalid = -1;

/// 256-entry table: byte value -> sextet (0..63), or kInvalid.
constexpr std::array<std::int8_t, 256> make_decode_table() {
    std::array<std::int8_t, 256> table{};
    table.fill(kInvalid);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

constexpr std::array<std::int8_t, 256> kDecodeTable = make_decode_table();

[[nodiscard]] int sextet(char c) noexcept {
    return kDecodeTable[static_cast<unsigned char>(c)];
}

}  // namespace

std::string base64_encode(ByteView data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {  // full groups: 3 bytes -> 4 characters
        const std::uint32_t group = (std::to_integer<std::uint32_t>(data[i]) << 16U) |
                                    (std::to_integer<std::uint32_t>(data[i + 1]) << 8U) |
                                    std::to_integer<std::uint32_t>(data[i + 2]);
        out += kAlphabet[(group >> 18U) & 0x3FU];
        out += kAlphabet[(group >> 12U) & 0x3FU];
        out += kAlphabet[(group >> 6U) & 0x3FU];
        out += kAlphabet[group & 0x3FU];
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t group = std::to_integer<std::uint32_t>(data[i]) << 16U;
        out += kAlphabet[(group >> 18U) & 0x3FU];
        out += kAlphabet[(group >> 12U) & 0x3FU];
        out += kPad;
        out += kPad;
    } else if (rest == 2) {
        const std::uint32_t group = (std::to_integer<std::uint32_t>(data[i]) << 16U) |
                                    (std::to_integer<std::uint32_t>(data[i + 1]) << 8U);
        out += kAlphabet[(group >> 18U) & 0x3FU];
        out += kAlphabet[(group >> 12U) & 0x3FU];
        out += kAlphabet[(group >> 6U) & 0x3FU];
        out += kPad;
    }
    return out;
}

namespace {

/// One validated group of 4 input characters.
struct Quartet {
    int a = 0;
    int b = 0;
    int c = 0;  // 0 when padded
    int d = 0;  // 0 when padded
    bool pad_c = false;
    bool pad_d = false;
};

/// Validate characters and padding shape of one group. `last_group`: padding is only legal at the
/// end.
Result<Quartet> parse_quartet(std::string_view group, bool last_group) {
    Quartet q;
    q.a = sextet(group[0]);
    q.b = sextet(group[1]);
    q.pad_c = group[2] == kPad;
    q.pad_d = group[3] == kPad;
    if ((q.pad_c || q.pad_d) && !last_group) {
        return fail(ErrorCode::InvalidBase64, "padding before the end");
    }
    if (q.pad_c && !q.pad_d) {
        return fail(ErrorCode::InvalidBase64, "malformed padding");
    }
    q.c = q.pad_c ? 0 : sextet(group[2]);
    q.d = q.pad_d ? 0 : sextet(group[3]);
    if (q.a == kInvalid || q.b == kInvalid || q.c == kInvalid || q.d == kInvalid) {
        return fail(ErrorCode::InvalidBase64, "invalid character");
    }
    return q;
}

/// Append the 1-3 bytes a group encodes, enforcing canonical (zero) unused trailing bits.
Result<void> append_bytes(const Quartet& q, std::vector<std::byte>& out) {
    const auto bits = (static_cast<std::uint32_t>(q.a) << 18U) |
                      (static_cast<std::uint32_t>(q.b) << 12U) |
                      (static_cast<std::uint32_t>(q.c) << 6U) | static_cast<std::uint32_t>(q.d);
    out.push_back(static_cast<std::byte>((bits >> 16U) & 0xFFU));
    if (q.pad_c) {  // "xx==": one byte; the 4 unused bits of the 2nd sextet must be zero
        return (q.b & 0x0F) == 0 ? Result<void>{}
                                 : fail(ErrorCode::InvalidBase64, "non-canonical trailing bits");
    }
    out.push_back(static_cast<std::byte>((bits >> 8U) & 0xFFU));
    if (q.pad_d) {  // "xxx=": two bytes; the 2 unused bits of the 3rd sextet must be zero
        return (q.c & 0x03) == 0 ? Result<void>{}
                                 : fail(ErrorCode::InvalidBase64, "non-canonical trailing bits");
    }
    out.push_back(static_cast<std::byte>(bits & 0xFFU));
    return {};
}

}  // namespace

Result<std::vector<std::byte>> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return fail(ErrorCode::InvalidBase64, "length is not a multiple of 4");
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const Result<Quartet> quartet = parse_quartet(text.substr(i, 4), i + 4 == text.size());
        if (!quartet) {
            return std::unexpected{quartet.error()};
        }
        if (const Result<void> appended = append_bytes(*quartet, out); !appended) {
            return std::unexpected{appended.error()};
        }
    }
    return out;
}

}  // namespace wsrv
