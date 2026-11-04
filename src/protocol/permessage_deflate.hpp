// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <span>
#include <string>

#include "protocol/extension_offer.hpp"

namespace wsrv {

/// Smallest and largest LZ77 window (log2 of bytes) the server will agree to. RFC 7692 allows 8
/// to 15, but zlib's raw deflate cannot do 8 (it silently uses 9), so the server never agrees to a
/// server window of 8 (ADR-0020 section 2).
inline constexpr int kMinDeflateWindowBits = 9;
inline constexpr int kMaxDeflateWindowBits = 15;

/// What the server is willing to do (operator configuration, ADR-0020 section 3).
struct DeflatePolicy {
    bool server_no_context_takeover = true;  ///< compressor is reset after every message
    int server_max_window_bits = 12;         ///< window of the server's compressor, 9..15
    int client_max_window_bits = 15;         ///< bound offered to clients that accept one, 9..15
};

/// @return true if both window sizes are inside 9..15.
[[nodiscard]] bool valid(const DeflatePolicy& policy) noexcept;

/// The parameters both sides agreed on; the codecs (extensions/) are built from this. "server"
/// always means the direction server to client, "client" the direction client to server.
struct DeflateParams {
    bool server_no_context_takeover = false;
    bool client_no_context_takeover = false;
    int server_window_bits = kMaxDeflateWindowBits;  ///< the server's compressor window
    int client_window_bits = kMaxDeflateWindowBits;  ///< the client's compressor window (our inflater always uses 15, which decodes any window)

    friend bool operator==(const DeflateParams&, const DeflateParams&) = default;
};

/// The outcome of a successful negotiation.
struct DeflateAgreement {
    DeflateParams params;
    std::string response;  ///< value for the response's Sec-WebSocket-Extensions header
};

/// Choose the first acceptable `permessage-deflate` offer (RFC 7692 sections 5 to 7). Offers for
/// other extensions are ignored and never echoed. An offer is unacceptable (the next one is
/// tried) if it has an unknown parameter, a bad or out-of-range value, or a value on a parameter
/// that takes none; if none is acceptable the result is empty and the connection simply proceeds
/// without compression, it is never an error (section 5.2).
/// `policy` must satisfy valid().
/// Thread-affinity: pure, any-thread.
[[nodiscard]] std::optional<DeflateAgreement>
negotiate_permessage_deflate(std::span<const ExtensionOffer> offers, const DeflatePolicy& policy);

}  // namespace wsrv
