// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wsrv {

/// One element of a client's Sec-WebSocket-Extensions header (RFC 6455 section 9.1, RFC 7692):
///   extension = token *( ";" param )     param = token [ "=" ( token / quoted-string ) ]
/// Quoted-string values are stored unquoted and unescaped. A parameter without a value has value
/// "".
struct ExtensionOffer {
    std::string name;
    std::vector<std::pair<std::string, std::string>> params;

    friend bool operator==(const ExtensionOffer&, const ExtensionOffer&) = default;
};

/// Work caps: duplicate detection is linear per item, so an unbounded list inside the 8 KiB head
/// would let one request cost milliseconds of loop-thread CPU (measured: ~6 ms for 2,500
/// parameters). Beyond these caps the header is treated as malformed.
inline constexpr std::size_t kMaxExtensionOffers = 16;  // per header line, and in total per request
inline constexpr std::size_t kMaxExtensionParams = 16;  // per offer

/// Parse the value of one Sec-WebSocket-Extensions header line (several header lines are parsed one
/// by one and concatenated). Strict: whitespace is allowed only around ';' and ',' (not around
/// '='), duplicate parameter names inside one offer, control characters, unterminated quotes, empty
/// names and junk after a value make the whole header malformed. Empty list elements (",,") are
/// ignored.
/// A quoted-string value must, once unescaped, be a token (RFC 6455 section 9.1), and more than
/// kMaxExtensionOffers offers or kMaxExtensionParams parameters is malformed (work caps, see
/// above).
/// @return the offers in order (possibly none), or std::nullopt if the header is malformed.
/// Thread-affinity: pure, any-thread.
[[nodiscard]] std::optional<std::vector<ExtensionOffer>>
parse_extension_offers(std::string_view header_value);

}  // namespace wsrv
