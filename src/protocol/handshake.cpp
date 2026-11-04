// SPDX-License-Identifier: Apache-2.0
#include "protocol/handshake.hpp"

#include "base/bytes.hpp"
#include "crypto/base64.hpp"
#include "crypto/sha1.hpp"
#include "http/token.hpp"
#include "protocol/handshake_rules.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace wsrv {

namespace {

constexpr std::string_view kAcceptGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";  // RFC 6455 section 1.3
constexpr std::string_view kInternalError =
    "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

constexpr int kFirstClientErrorStatus = 400;
constexpr int kLastServerErrorStatus = 599;

/// Headers the server itself owns: a user rule must not set them (framing, connection management,
/// upgrade and handshake semantics).
[[nodiscard]] bool is_server_owned_header(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 4> kOwned{
        "Connection", "Content-Length", "Transfer-Encoding", "Upgrade"};
    constexpr std::string_view kHandshakePrefix = "Sec-WebSocket-";
    return std::ranges::any_of(kOwned,
                               [name](std::string_view owned) { return iequals(name, owned); }) ||
           (name.size() >= kHandshakePrefix.size() &&
            iequals(name.substr(0, kHandshakePrefix.size()), kHandshakePrefix));
}

/// A refusal produced by application code is untrusted until it passes this check.
[[nodiscard]] bool is_acceptable_user_refusal(const HandshakeReject& refusal) noexcept {
    if (refusal.status < kFirstClientErrorStatus || refusal.status > kLastServerErrorStatus) {
        return false;
    }
    return std::ranges::all_of(refusal.headers, [](const HttpHeader& header) {
        return is_token(header.name) && is_valid_header_value(header.value) &&
               !is_server_owned_header(header.name);
    });
}

void validate_config(const HandshakeConfig& config) {
    if (std::ranges::any_of(config.user_rules, [](const auto& rule) { return rule == nullptr; })) {
        throw std::invalid_argument("HandshakeConfig: user rule must not be null");
    }
    if (config.permessage_deflate && !valid(*config.permessage_deflate)) {
        throw std::invalid_argument("HandshakeConfig: permessage-deflate window bits must be 9..15");
    }
    for (std::size_t i = 0; i < config.subprotocols.size(); ++i) {
        const std::string& name = config.subprotocols[i];
        const bool repeated =
            std::ranges::find(config.subprotocols.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                              config.subprotocols.end(),
                              name) != config.subprotocols.end();
        if (!is_token(name) || repeated) {
            throw std::invalid_argument("HandshakeConfig: subprotocol '" + name +
                                        "' is not a unique token");
        }
    }
    for (const std::string& origin : config.allowed_origins) {
        const bool plausible =
            origin == "null" ||
            (origin.find("://") != std::string::npos && origin.back() != '/' &&
             is_valid_header_value(origin) && origin.find(' ') == std::string::npos);
        if (!plausible) {
            throw std::invalid_argument(
                "HandshakeConfig: allowed origin '" + origin +
                "' can never match (expected scheme://host[:port], no trailing slash)");
        }
    }
}

}  // namespace

std::string compute_accept_key(std::string_view client_key) {
    std::string input{client_key};
    input += kAcceptGuid;
    const Sha1::Digest digest = Sha1::hash(as_bytes(input));
    return base64_encode(digest);
}

HandshakeNegotiator::HandshakeNegotiator(HandshakeConfig config)
    : config_{(validate_config(config), std::move(config))},
      builtin_rules_{make_builtin_rules(config_)} {}

HandshakeDecision HandshakeNegotiator::negotiate(const HttpRequest& request) const {
    HandshakeContext context;
    for (const auto& rule : builtin_rules_) {
        if (std::optional<HandshakeReject> refusal = rule->evaluate(request, context)) {
            return std::move(*refusal);
        }
    }
    // User rules may only add annotations; the negotiated facts are frozen at this point.
    const std::string chosen_subprotocol = context.subprotocol;
    const std::vector<ExtensionOffer> chosen_offers = context.extension_offers;
    for (const auto& rule : config_.user_rules) {
        try {  // exception barrier: application code must never take the server down (FR-S6)
            if (std::optional<HandshakeReject> refusal = rule->evaluate(request, context)) {
                if (!is_acceptable_user_refusal(*refusal)) {
                    return HandshakeReject{500, {}, false};
                }
                return std::move(*refusal);
            }
        } catch (
            ...) {  // NOLINT(bugprone-empty-catch): flagged in the decision, logged by the caller
            return HandshakeReject{500, {}, true};
        }
        if (context.subprotocol != chosen_subprotocol ||
            context.extension_offers != chosen_offers) {
            return HandshakeReject{500, {}, false};
        }
    }
    // check_key guaranteed exactly one valid key header.
    const std::string_view key = request.values("Sec-WebSocket-Key").front();
    std::optional<DeflateAgreement> deflate;
    if (config_.permessage_deflate) {  // RFC 7692: pure choice over the offers, never an error
        deflate = negotiate_permessage_deflate(context.extension_offers, *config_.permessage_deflate);
    }
    return HandshakeAccept{compute_accept_key(key),
                           std::move(context.subprotocol),
                           std::move(context.extension_offers),
                           std::move(deflate),
                           std::move(context.annotations)};
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
        if (accept->deflate) {  // the only extension text the server ever sends
            response.headers.push_back({"Sec-WebSocket-Extensions", accept->deflate->response});
        }
        return response;
    }
    const auto& reject = std::get<HandshakeReject>(decision);
    response.status = reject.status;
    response.headers = reject.headers;
    const bool offers_upgrade = std::ranges::any_of(
        reject.headers, [](const HttpHeader& header) { return iequals(header.name, "Upgrade"); });
    response.headers.push_back({"Connection", offers_upgrade ? "Upgrade, close" : "close"});
    response.headers.push_back({"Content-Length", "0"});
    return response;
}

std::string render_response(const HandshakeDecision& decision) {
    Result<std::string> text = serialize(to_response(decision));
    return text.has_value() ? std::move(*text) : std::string{kInternalError};
}

}  // namespace wsrv
