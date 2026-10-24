// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "http/request.hpp"
#include "http/response.hpp"
#include "protocol/extension_offer.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wsrv {

/// Sec-WebSocket-Accept = Base64( SHA-1( client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
/// (RFC 6455 section 4.2.2). `client_key` must already have been validated. Thread-affinity:
/// any-thread.
[[nodiscard]] std::string compute_accept_key(std::string_view client_key);

/// The server's answer when it refuses the upgrade. `headers` are extra headers for the response
/// (e.g. Allow, Sec-WebSocket-Version); to_response() adds Connection: close and Content-Length: 0.
struct HandshakeReject {
    int status = 400;
    std::vector<HttpHeader> headers;
    bool from_exception =
        false;  // set when a user rule threw: the connection layer should log it (FR-S6)
};

/// Work cap on the number of distinct subprotocol offers (duplicate detection is linear per token).
inline constexpr std::size_t kMaxSubprotocolOffers = 32;

/// Facts the rules collect for the final decision.
struct HandshakeContext {
    std::string subprotocol;                       // chosen by the built-in rule; empty = none
    std::vector<ExtensionOffer> extension_offers;  // valid offers; none are accepted before M9
    /// Application data set by user rules (user id, claims...), handed to the application with the
    /// accepted connection and never sent to the peer. This is the ONLY part of the context user
    /// rules may change: if a rule alters `subprotocol` or `extension_offers` the handshake fails
    /// with 500.
    std::map<std::string, std::string> annotations;
};

/// One link of the handshake validation chain (Chain of Responsibility). Returns std::nullopt to
/// let the next rule run, or a HandshakeReject to stop the chain with that answer.
///
/// Thread-affinity: one rule instance is shared by all connections and may be called concurrently
/// from several loop threads, so evaluate() must be thread-safe (no mutable state without
/// synchronisation). Rules may block only if the application accepts the loop-lag cost; slow
/// authentication belongs in an asynchronous hook (later milestone).
class IHandshakeRule {
public:
    IHandshakeRule() = default;
    IHandshakeRule(const IHandshakeRule&) = delete;
    IHandshakeRule& operator=(const IHandshakeRule&) = delete;
    IHandshakeRule(IHandshakeRule&&) = delete;
    IHandshakeRule& operator=(IHandshakeRule&&) = delete;
    virtual ~IHandshakeRule() = default;

    [[nodiscard]] virtual std::optional<HandshakeReject>
    evaluate(const HttpRequest& request, HandshakeContext& context) const = 0;
};

struct HandshakeConfig {
    /// Allowed values of the Origin header, compared ASCII case-insensitively and exactly
    /// (scheme://host[:port]). With an empty list, any request that carries an Origin is refused
    /// (403).
    std::vector<std::string> allowed_origins;
    /// Accept every Origin (and a missing one). Deliberately loud: it disables the protection
    /// against cross-site WebSocket hijacking (SECURITY_DESIGN T8).
    bool allow_any_origin = false;
    /// Accept requests without an Origin header (non-browser clients). Browsers always send one.
    bool allow_missing_origin = false;
    /// Subprotocols the server speaks, in order of preference.
    std::vector<std::string> subprotocols;
    /// Refuse (400) when the client offers none of `subprotocols`.
    bool require_subprotocol = false;
    /// Application rules (authentication, rate limits...), run after all built-in rules, in order.
    std::vector<std::shared_ptr<const IHandshakeRule>> user_rules;
};

struct HandshakeAccept {
    std::string accept_key;   // value of Sec-WebSocket-Accept
    std::string subprotocol;  // empty = none selected
    std::vector<ExtensionOffer> extension_offers;
    std::map<std::string, std::string> annotations;  // from user rules; for the application only
};

using HandshakeDecision = std::variant<HandshakeAccept, HandshakeReject>;

/// Validates an upgrade request and decides. Built-in rules run in this order, the first refusal
/// wins: method, HTTP version, Host (valid host[:port]), no body, Upgrade, Connection,
/// Sec-WebSocket-Key, Sec-WebSocket-Version, Origin, subprotocols, extension offers; then the
/// configured user rules inside an exception barrier. A user rule's refusal is validated before it
/// is used: status 400..599, token header names, clean values, and none of the headers the server
/// owns (Connection, Content-Length, Transfer-Encoding, Upgrade, Sec-WebSocket-*); anything else
/// becomes a 500. A throwing rule becomes a 500 with `from_exception` set; a rule that changes the
/// negotiated subprotocol or extension offers also becomes a 500. Pure: no I/O, no clock.
/// Thread-affinity: negotiate() is const and safe to call concurrently if the user rules are.
class HandshakeNegotiator {
public:
    /// @throws std::invalid_argument for an invalid configuration (null rule, subprotocol that is
    /// not a token or is repeated, allowed origin that cannot match such as "*", a trailing slash
    /// or a missing scheme). Config errors are startup errors (ADR-0006): this is the one place the
    /// handshake throws.
    explicit HandshakeNegotiator(HandshakeConfig config);

    [[nodiscard]] HandshakeDecision negotiate(const HttpRequest& request) const;

private:
    HandshakeConfig config_;
    std::vector<std::unique_ptr<IHandshakeRule>> builtin_rules_;
};

/// The HTTP response to send for `decision`: 101 with Upgrade/Connection/Sec-WebSocket-Accept (and
/// Sec-WebSocket-Protocol if one was selected), or the rejection plus Connection: close
/// (Connection: Upgrade, close when the rejection carries an Upgrade header, as RFC 9110 requires
/// for 426) and Content-Length: 0. Extension offers and annotations are never echoed.
[[nodiscard]] HttpResponse to_response(const HandshakeDecision& decision);

/// The exact bytes to send for `decision`; never fails: if the response cannot be serialised (which
/// validated decisions never cause) a fixed, well-formed 500 is returned instead of nothing.
[[nodiscard]] std::string render_response(const HandshakeDecision& decision);

}  // namespace wsrv
