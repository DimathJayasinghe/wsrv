// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "http/request.hpp"
#include "net/endpoint.hpp"
#include "server/handler.hpp"
#include "server/handshake_verdict.hpp"
#include "server/session_handle.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wsrv {

/// Size caps on what is copied to the application (ADR-0018 section 2). Over a cap the upgrade is
/// refused with 431: a credential is never silently cut.
inline constexpr std::size_t kMaxForwardedValueBytes = 4096;     // 4 KiB
inline constexpr std::size_t kMaxHandshakeRequestBytes = 16384;  // 16 KiB

/// What the application is told about a valid upgrade that waits for its decision. All values are
/// copies; Cookie and Authorization appear in `forwarded` only if the operator listed them.
struct HandshakeRequest {
    ConnectionId id = 0;
    Endpoint peer;
    std::string path;   ///< request target up to '?'
    std::string query;  ///< after '?', raw (not decoded); empty if none
    std::string origin;
    std::string host;
    std::vector<std::string> offered_subprotocols;  ///< what the client offered, in order
    std::string subprotocol;                        ///< what the server's own rules negotiated
    std::vector<HttpHeader> forwarded;              ///< listed headers, configured spelling
};

/// What the server knows about an upgrade that waits (a parameter object).
struct HandshakeFacts {
    ConnectionId id;
    const Endpoint& peer;
    const HttpRequest& request;  ///< already passed the built-in rules
    std::string_view negotiated_subprotocol;
};

/// Describe an accepted-but-undecided upgrade. @return nothing if a size cap is exceeded (the
/// caller refuses with 431).
[[nodiscard]] std::optional<HandshakeRequest>
make_handshake_request(const HandshakeFacts& facts,
                       const std::vector<std::string>& forward_headers);

/// How the application answers one waiting upgrade, from any thread. Copyable and cheap, holds the
/// connection's id and the loop's link, never a pointer to the connection (like SessionHandle): an
/// answer for a connection that is gone, or already decided, is dropped on the loop.
/// accept() and reject() check their arguments first, on the caller's thread, so a mistake is
/// reported to the caller and the connection keeps waiting (until the timeout).
class PendingHandle {
public:
    PendingHandle() = default;
    PendingHandle(std::shared_ptr<LoopLink> link,
                  ConnectionId id,
                  std::shared_ptr<const std::vector<std::string>> offered) noexcept
        : link_{std::move(link)}, id_{id}, offered_{std::move(offered)} {}

    [[nodiscard]] ConnectionId id() const noexcept {
        return id_;
    }

    /// Let the client in. `subprotocol`, if given, must be one the client offered
    /// (Error{InvalidArgument} otherwise); without it the server's negotiated one is kept.
    [[nodiscard]] Result<void> accept(std::optional<std::string_view> subprotocol = {}) const;
    /// Refuse with `status`, one of kDecisionStatuses (Error{InvalidArgument} otherwise).
    [[nodiscard]] Result<void> reject(int status) const;

private:
    [[nodiscard]] Result<void> send(HandshakeVerdict verdict) const;

    std::shared_ptr<LoopLink> link_;
    ConnectionId id_ = 0;
    std::shared_ptr<const std::vector<std::string>> offered_;
};

/// Told about every upgrade that waits. Implemented by the host side (capi `HostBridge`); the
/// server never knows what is behind it (ADR-0002 style seam).
///
/// The call happens on a loop thread and must not block or run application code: queue the request
/// for the application and return. Thread-affinity: request() may be called from several loop
/// threads at once.
class IHandshakeDecider {
public:
    IHandshakeDecider() = default;
    IHandshakeDecider(const IHandshakeDecider&) = delete;
    IHandshakeDecider& operator=(const IHandshakeDecider&) = delete;
    IHandshakeDecider(IHandshakeDecider&&) = delete;
    IHandshakeDecider& operator=(IHandshakeDecider&&) = delete;
    virtual ~IHandshakeDecider() = default;

    /// @return Ok if the request was taken and `handle` will be used to answer; an Error (queue
    /// full, nobody to ask) makes the server refuse the upgrade with 503 at once. Failing closed:
    /// "cannot ask" never means "accept".
    [[nodiscard]] virtual Result<void> request(HandshakeRequest request, PendingHandle handle) = 0;
};

}  // namespace wsrv
