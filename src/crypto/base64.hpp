// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wsrv {

/// Encode `data` as standard Base64 (RFC 4648 section 4) with mandatory '=' padding and no line
/// breaks. Thread-affinity: any-thread.
[[nodiscard]] std::string base64_encode(ByteView data);

/// Strictly decode standard Base64. Accepts exactly the canonical encoding and nothing else:
///  - length is a multiple of 4; alphabet A-Za-z0-9+/ only (no whitespace, NUL, URL-safe '-' '_');
///  - '=' padding only as the last one or two characters, and mandatory when needed;
///  - unused trailing bits are zero (so each byte string has exactly one valid encoding).
/// Lenient decoders are a source of parser-differential attacks; do not add leniency here.
/// @return the bytes, or Error{InvalidBase64}. The output is at most 3/4 of the input size, so
/// memory use is bounded by the (already bounded) input. Thread-affinity: any-thread.
[[nodiscard]] Result<std::vector<std::byte>> base64_decode(std::string_view text);

}  // namespace wsrv
