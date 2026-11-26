// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "base/bytes.hpp"

namespace wsrv::wsrvd {

/// The IPC token form over TCP (ADR-0038 section 3): `wsrv1_`, 64 lowercase hex digits (32 bytes
/// from the kernel's random source, 256 bits), `_`, and 8 lowercase hex digits of the CRC-32 of
/// everything before that last underscore. 79 bytes in all.
///
/// Why a form and not just a length: a length floor lets "MyCompanyWebsocketToken2026!!!!!" through,
/// and over TCP the token is the only authentication. Only `wsrvd --generate-token` writes this
/// form, so a token in it is random unless someone forged one on purpose. The checksum is no secret
/// and no defence against that forger; it catches the operator who retypes, truncates or edits a
/// token by hand. A new form later gets a new prefix (`wsrv2_`) accepted next to this one.
inline constexpr std::size_t kGeneratedTokenRandomBytes = 32;
inline constexpr std::size_t kGeneratedTokenSize = 6 + (2 * kGeneratedTokenRandomBytes) + 1 + 8;

/// CRC-32 (IEEE 802.3, the one zlib and Ethernet use): check value of "123456789" is 0xCBF43926.
[[nodiscard]] std::uint32_t crc32(ByteView data) noexcept;

/// The token for these bytes. Precondition: `random.size() == kGeneratedTokenRandomBytes`.
[[nodiscard]] std::string format_generated_token(ByteView random);

/// Whether `token` is exactly that form with a matching checksum (a trailing newline is the file
/// reader's business and is not accepted here).
[[nodiscard]] bool is_generated_token(std::string_view token) noexcept;

}  // namespace wsrv::wsrvd
