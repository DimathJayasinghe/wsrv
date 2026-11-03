// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "server/handshake_decider.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace wsrv {

/// A waiting upgrade as it travels inside a HostEvent::data (ADR-0018 section 5). One encoding,
/// read by the C layer and by wsrvd, so the two cannot drift apart. All integers are big-endian:
///
///   string peer, path, query, origin, host, subprotocol      (u32 length + bytes, each)
///   u16 offered count, then that many strings
///   u16 header count, then that many (name string, value string)
///
/// Lengths, not separators, delimit every field, so any byte value is carried faithfully.
[[nodiscard]] std::vector<std::byte> encode_handshake(std::string_view peer,
                                                      const HandshakeRequest& request);

/// The decoded fields, as views into the buffer given to decode_handshake (valid while it is).
struct HandshakeView {
    std::string_view peer;  ///< "host:port"
    std::string_view path;
    std::string_view query;
    std::string_view origin;
    std::string_view host;
    std::string_view subprotocol;  ///< what the server's rules negotiated; may be empty
    std::vector<std::string_view> offered;
    std::vector<std::pair<std::string_view, std::string_view>> headers;
};

/// @return nothing for anything that is not exactly one well-formed blob: a length beyond the
/// buffer, a truncation, trailing bytes. Never reads outside `blob`.
[[nodiscard]] std::optional<HandshakeView> decode_handshake(ByteView blob);

}  // namespace wsrv
