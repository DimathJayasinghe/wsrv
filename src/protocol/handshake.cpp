// SPDX-License-Identifier: Apache-2.0
#include "protocol/handshake.hpp"

#include "base/bytes.hpp"
#include "crypto/base64.hpp"
#include "crypto/sha1.hpp"
#include "protocol/handshake_rules.hpp"

#include <utility>

namespace wsrv {

namespace {
constexpr std::string_view kAcceptGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";  // RFC 6455 section 1.3
}  // namespace

std::string compute_accept_key(std::string_view client_key) {
    std::string input{client_key};
    input += kAcceptGuid;
    const Sha1::Digest digest = Sha1::hash(as_bytes(input));
    return base64_encode(digest);
}

HandshakeNegotiator::HandshakeNegotiator(HandshakeConfig config)
    : config_{std::move(config)}, builtin_rules_{make_builtin_rules(config_)} {}

HandshakeDecision HandshakeNegotiator::negotiate(const HttpRequest& request) const {
    HandshakeContext context;
    for (const auto& rule : builtin_rules_) {
        if (std::optional<HandshakeReject> refusal = rule->evaluate(request, context)) {
            return std::move(*refusal);
        }
    }
    for (const auto& rule : config_.user_rules) {
        try {  // exception barrier: application code must never take the server down (FR-S6)
            if (std::optional<HandshakeReject> refusal = rule->evaluate(request, context)) {
                return std::move(*refusal);
            }
        } catch (
            ...) {  // NOLINT(bugprone-empty-catch): converted into a 500 below, nothing to log here
            return HandshakeReject{500, {}};
        }
    }
    // check_key guaranteed exactly one valid key header.
    const std::string_view key = request.values("Sec-WebSocket-Key").front();
    return HandshakeAccept{compute_accept_key(key),
                           std::move(context.subprotocol),
                           std::move(context.extension_offers)};
}

HttpResponse to_response(const HandshakeDecision& decision) {
    HttpResponse response;
    if (const auto* accept = std::get_if<HandshakeAccept>(&decision)) {
        response.status = 101;
        response.headers = {{"Upgrade", "websocket"},
                            {"Connection", "Upgrade"},
                            {"Sec-WebSocket-Accept", accept->accept_key}};
        if (!accept->subprotocol.empty()) {
            response.headers.push_back({"Sec-WebSocket-Protocol", accept->subprotocol});
        }
        return response;
    }
    const auto& reject = std::get<HandshakeReject>(decision);
    response.status = reject.status;
    response.headers = reject.headers;
    response.headers.push_back({"Connection", "close"});
    response.headers.push_back({"Content-Length", "0"});
    return response;
}

}  // namespace wsrv
