// SPDX-License-Identifier: Apache-2.0
#include "protocol/close_payload.hpp"

#include "protocol/utf8.hpp"

#include <algorithm>

namespace wsrv {

FrameResult<std::optional<ClosePayload>> parse_close_payload(ByteView payload) {
    if (payload.empty()) {
        return std::optional<ClosePayload>{};  // "no status received"
    }
    if (payload.size() == 1) {
        return protocol_failure(CloseCode::ProtocolError, "close payload of exactly 1 byte");
    }
    const std::uint16_t code = load_be16(payload);
    if (!is_valid_on_wire(code)) {
        return protocol_failure(CloseCode::ProtocolError, "invalid close status code");
    }
    const ByteView reason_bytes = payload.subspan(2);
    if (reason_bytes.size() > kMaxCloseReason) {
        return protocol_failure(CloseCode::ProtocolError, "close reason too long");
    }
    if (!is_valid_utf8(reason_bytes)) {
        return protocol_failure(CloseCode::InvalidPayload, "close reason is not valid UTF-8");
    }
    return ClosePayload{code, as_chars(reason_bytes)};
}

std::size_t encode_close_payload(MutableByteView out, CloseCode code, std::string_view reason) {
    WSRV_ASSERT(is_valid_on_wire(static_cast<std::uint16_t>(code)));
    WSRV_ASSERT(out.size() >= kMaxControlPayload);
    store_be16(out, static_cast<std::uint16_t>(code));
    std::size_t length = std::min(reason.size(), kMaxCloseReason);
    // Never cut a multi-byte character: back up while the first dropped byte is a continuation
    // byte.
    while (length > 0 && length < reason.size() &&
           (static_cast<unsigned char>(reason[length]) & 0xC0U) == 0x80U) {
        --length;
    }
    const ByteView text = as_bytes(reason).first(length);
    std::copy(text.begin(), text.end(), out.begin() + 2);
    return 2 + length;
}

}  // namespace wsrv
