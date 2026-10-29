// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "net/endpoint.hpp"
#include "protocol/close_code.hpp"
#include "protocol/message_assembler.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace wsrv {

/// Process-unique, never reused within a run: the key for logs, tables and (later) cross-thread
/// handles. A counter, not a descriptor number, so a reused fd can never be mistaken for it
/// (EC-N12).
using ConnectionId = std::uint64_t;

/// Why and how a connection ended, reported once to IWebSocketHandler::on_close.
struct CloseInfo {
    CloseCode code =
        CloseCode::AbnormalClosure;  ///< the peer's code, ours, or 1006 (no Close seen)
    std::string reason;              ///< UTF-8; empty if none
    bool clean = false;              ///< the closing handshake completed
};

/// What a handler may do with one connection. Implemented by the server's Connection; handlers
/// only call it.
///
/// Valid only on the connection's loop thread and only inside a handler callback: do not store the
/// reference (ADR-0013, ARCHITECTURE section 4). After the connection started closing, send_*
/// return Error{Closing} and write nothing (EC-C14); close() is idempotent (EC-C15). close() only
/// records the request; the work happens after the current callback returns (EC-A06).
/// Thread-affinity: loop-thread-only.
class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    virtual ~Session() = default;

    [[nodiscard]] virtual ConnectionId id() const noexcept = 0;
    [[nodiscard]] virtual const Endpoint& peer() const noexcept = 0;

    /// Queue one complete message; large ones are fragmented automatically (FR-F9). The data is
    /// copied before the call returns, so views from a callback argument may be passed straight
    /// back (echo).
    /// @return Error{Closing}, Error{InvalidUtf8} (text), Error{Backpressure} or Error{BufferFull}
    /// when the outbound queue cannot take it; nothing is queued on error.
    [[nodiscard]] virtual Result<void> send_text(std::string_view text) = 0;
    [[nodiscard]] virtual Result<void> send_binary(ByteView data) = 0;

    /// Start the closing handshake with `code` (must be sendable, RFC 6455 section 7.4) and an
    /// optional reason (cut to 123 bytes).
    virtual void close(CloseCode code, std::string_view reason) = 0;
};

/// The application's side of a connection (Observer). Every callback is optional.
///
/// Rules: callbacks run on the loop thread and must not block (EC-A02); `string_view`/`ByteView`
/// arguments are valid for the duration of the call only (EC-A04); a callback must not outlive
/// or destroy the server (EC-A05). A thrown exception is caught at the boundary: the connection is
/// closed with 1011 and the server keeps running (FR-S6, EC-A01).
/// Order: on_open at most once; then messages; then on_close exactly once if on_open ran.
/// Thread-affinity: loop-thread-only (one instance per connection, see IHandlerFactory).
class IWebSocketHandler {
public:
    IWebSocketHandler() = default;
    IWebSocketHandler(const IWebSocketHandler&) = delete;
    IWebSocketHandler& operator=(const IWebSocketHandler&) = delete;
    IWebSocketHandler(IWebSocketHandler&&) = delete;
    IWebSocketHandler& operator=(IWebSocketHandler&&) = delete;
    virtual ~IWebSocketHandler() = default;

    virtual void on_open(Session& /*session*/) {}
    /// Buffered delivery (the default): a complete text message, valid UTF-8.
    virtual void on_text(Session& /*session*/, std::string_view /*text*/) {}
    /// Buffered delivery: a complete binary message.
    virtual void on_binary(Session& /*session*/, ByteView /*data*/) {}
    /// Streaming delivery (`AssemblerConfig::streaming`): one part of a message. Text parts end on
    /// UTF-8 code-point boundaries (FRAGMENTATION section 1.3).
    virtual void
    on_message_part(Session& /*session*/, MessageType /*type*/, ByteView /*part*/, bool /*final*/) {
    }
    /// A fault that does not by itself end the connection (the server logs it too).
    virtual void on_error(Session& /*session*/, const Error& /*error*/) {}
    virtual void on_close(Session& /*session*/, const CloseInfo& /*info*/) {}
};

/// What the server knows about a connection when its handler is created (after the handshake
/// succeeded, before on_open).
struct ConnectionInfo {
    ConnectionId id = 0;
    Endpoint peer;
    std::string subprotocol;  ///< negotiated; empty = none
    std::map<std::string, std::string>
        annotations;  ///< set by handshake rules (user id, claims...)
};

/// Creates one handler per connection (Factory), so handlers keep per-connection state without
/// locks. Called on the loop thread; must not throw (a null result closes the connection with
/// 1011).
/// Thread-affinity: may be called concurrently from several loop threads (M7): no shared mutable
/// state without synchronisation.
class IHandlerFactory {
public:
    IHandlerFactory() = default;
    IHandlerFactory(const IHandlerFactory&) = delete;
    IHandlerFactory& operator=(const IHandlerFactory&) = delete;
    IHandlerFactory(IHandlerFactory&&) = delete;
    IHandlerFactory& operator=(IHandlerFactory&&) = delete;
    virtual ~IHandlerFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<IWebSocketHandler> create(const ConnectionInfo& info) = 0;
};

}  // namespace wsrv
