// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: for any bytes the parser accepts, the negotiator must decide without crashing
// and the response we would send must be well formed (no injected header lines, correct framing).
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "crypto/base64.hpp"
#include "http/request_parser.hpp"
#include "protocol/handshake.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace {

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string_view::npos;
         pos = text.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

void check_response(const wsrv::HandshakeDecision& decision) {
    const wsrv::HttpResponse response = wsrv::to_response(decision);
    const auto text = wsrv::serialize(response);
    WSRV_ASSERT(text.has_value());  // every decision we can make must be serialisable
    // Framing: status line + one line per header + blank line, each ended by exactly one CRLF,
    // nothing injected.
    WSRV_ASSERT(count_occurrences(*text, "\r\n") == response.headers.size() + 2);
    WSRV_ASSERT(text->ends_with("\r\n\r\n"));
    WSRV_ASSERT(count_occurrences(*text, "\n") == count_occurrences(*text, "\r\n"));
    if (std::holds_alternative<wsrv::HandshakeAccept>(decision)) {
        WSRV_ASSERT(text->starts_with("HTTP/1.1 101 "));
        WSRV_ASSERT(text->find("Sec-WebSocket-Extensions") ==
                    std::string::npos);  // offers are never echoed
    } else {
        WSRV_ASSERT(text->find("Connection: close\r\n") != std::string::npos);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView all = std::as_bytes(std::span<const std::uint8_t>{data, size});
    wsrv::HttpRequestParser parser;
    const auto progress = parser.feed(all);
    if (!progress || !progress->complete) {
        return 0;
    }
    const wsrv::HttpRequest& request = parser.request();

    wsrv::HandshakeConfig strict;  // secure defaults
    wsrv::HandshakeConfig
        open;  // permissive origin policy, a subprotocol list, strict subprotocol requirement
    open.allow_any_origin = true;
    open.subprotocols = {"chat", "superchat"};
    open.require_subprotocol = true;

    for (const wsrv::HandshakeConfig& config : {strict, open}) {
        const wsrv::HandshakeDecision decision =
            wsrv::HandshakeNegotiator{config}.negotiate(request);
        check_response(decision);
        if (const auto* accept = std::get_if<wsrv::HandshakeAccept>(&decision)) {
            // An accept implies a well-formed single key header (cross-check the rule independently
            // here).
            const auto keys = request.values("Sec-WebSocket-Key");
            WSRV_ASSERT(keys.size() == 1);
            const auto decoded = wsrv::base64_decode(keys.front());
            WSRV_ASSERT(decoded.has_value() && decoded->size() == 16);
            WSRV_ASSERT(accept->accept_key == wsrv::compute_accept_key(keys.front()));
        }
    }
    return 0;
}
