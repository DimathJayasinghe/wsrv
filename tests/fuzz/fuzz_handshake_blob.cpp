// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the handshake question blob (ADR-0018 amendment 2). The blob crosses a queue
// and, in wsrvd, a socket, and is decoded by the C layer, so the decoder must be safe for any
// bytes. Properties: (1) it never reads outside the input (ASan); (2) the format is canonical:
// whatever decodes, encodes back to exactly the same bytes, so the encoder and decoder cannot
// disagree.
#include "base/assert.hpp"
#include "base/bytes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "capi/handshake_blob.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as std::byte, same size.
    const wsrv::ByteView blob{reinterpret_cast<const std::byte*>(data), size};
    const auto view = wsrv::decode_handshake(blob);
    if (!view) {
        return 0;
    }
    wsrv::HandshakeRequest request;
    request.path = std::string{view->path};
    request.query = std::string{view->query};
    request.origin = std::string{view->origin};
    request.host = std::string{view->host};
    request.subprotocol = std::string{view->subprotocol};
    for (const std::string_view offered : view->offered) {
        request.offered_subprotocols.emplace_back(offered);
    }
    for (const auto& [name, value] : view->headers) {
        request.forwarded.push_back(wsrv::HttpHeader{std::string{name}, std::string{value}});
    }
    const std::vector<std::byte> again = wsrv::encode_handshake(view->peer, request);
    WSRV_ASSERT(again.size() == size);
    WSRV_ASSERT(std::equal(again.begin(), again.end(), blob.begin()));
    return 0;
}
