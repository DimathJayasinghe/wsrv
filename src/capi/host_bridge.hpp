// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "server/handler.hpp"
#include "server/session_handle.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "capi/event_queue.hpp"

namespace wsrv {

/// The embedding seam (ADR-0016): turns what happens on the loop threads into events a host
/// thread can pull, and lets the host act on connections by id from its own thread.
///
/// It is the whole of "what a binding needs from the core", written once in C++ so that every
/// binding (PHP, C#, Java, Node...) is only a translation of types: the socket and state edge
/// cases are handled here and below, never in a binding.
///   - Every Open is followed by exactly one Close, so the host can always free what it kept.
///   - Events of one connection arrive in order: Open, messages, Close.
///   - A host that stops polling cannot grow memory: past the queue limit a connection is closed
///     with 1013 (TryAgainLater) and its events are dropped, never the Close.
///   - A send to a connection that is gone, or never existed, fails with Closing; it never
///     touches freed memory, whatever the timing (EC-N13).
/// Plug it in as the server's IHandlerFactory; poll events() from the host.
/// Thread-affinity: the factory part on loop threads; send_*, close, events() any thread, with a
/// single consumer thread for events().try_pop().
class HostBridge final : public IHandlerFactory {
public:
    /// @return Error from EventQueue::create.
    [[nodiscard]] static Result<std::unique_ptr<HostBridge>> make(ISyscalls& sys,
                                                                  EventQueueConfig queue);

    /// Construction goes through make() (passkey, so make_unique can call it).
    class Key {
        friend class HostBridge;
        Key() = default;
    };
    HostBridge(Key /*unused*/, std::unique_ptr<EventQueue> events) noexcept
        : events_{std::move(events)} {}

    [[nodiscard]] std::unique_ptr<IWebSocketHandler> create(const ConnectionInfo& info) override;

    /// The events for the host. Valid for the bridge's lifetime.
    [[nodiscard]] EventQueue& events() noexcept {
        return *events_;
    }

    /// Queue a message for connection `id`; "accepted" means handed to its loop, not delivered
    /// (SessionHandle). Text is checked as UTF-8 here, so the host learns of a bad string at once.
    /// @return Error{Closing} for an unknown or finished connection, Error{InvalidUtf8},
    /// Error{BufferFull}, Error{ResourceExhausted}.
    [[nodiscard]] Result<void> send_text(ConnectionId id, std::string_view text);
    [[nodiscard]] Result<void> send_binary(ConnectionId id, ByteView data);
    /// Start the closing handshake of connection `id` (same errors as send_text).
    [[nodiscard]] Result<void> close(ConnectionId id, CloseCode code, std::string_view reason);

private:
    class Handler;

    void add(ConnectionId id, SessionHandle handle);
    void remove(ConnectionId id);
    [[nodiscard]] std::optional<SessionHandle> find(ConnectionId id);

    const std::unique_ptr<EventQueue> events_;
    std::mutex mutex_;  // guards handles_; held for one map operation, never across a call out
    std::unordered_map<ConnectionId, SessionHandle> handles_;
};

}  // namespace wsrv
