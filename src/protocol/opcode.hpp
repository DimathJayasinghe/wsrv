// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wsrv {

/// Frame opcodes (RFC 6455 section 5.2). 0x3-0x7 and 0xB-0xF are reserved and make a frame invalid.
enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/// Control frames (Close, Ping, Pong) have the high bit of the opcode set.
[[nodiscard]] constexpr bool is_control(Opcode opcode) noexcept {
    return (static_cast<std::uint8_t>(opcode) & 0x8U) != 0;
}

/// The Opcode for a wire value, or nullopt for a reserved / out-of-range value. Pure, any-thread.
[[nodiscard]] constexpr std::optional<Opcode> opcode_from_wire(std::uint8_t value) noexcept {
    switch (value) {
    case 0x0:
        return Opcode::Continuation;
    case 0x1:
        return Opcode::Text;
    case 0x2:
        return Opcode::Binary;
    case 0x8:
        return Opcode::Close;
    case 0x9:
        return Opcode::Ping;
    case 0xA:
        return Opcode::Pong;
    default:
        return std::nullopt;
    }
}

/// Control frame payloads are at most 125 bytes (RFC 6455 section 5.5).
inline constexpr std::size_t kMaxControlPayload = 125;

}  // namespace wsrv
