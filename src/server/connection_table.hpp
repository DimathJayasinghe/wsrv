// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/event_loop.hpp"
#include "server/connection.hpp"
#include "server/loop_host.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>

namespace wsrv {

/// Owns the live connections of one loop, keyed by connection id (never by descriptor number,
/// EC-N12). A finished connection asks to be retired from inside its own callback; the table
/// destroys it later, at EventLoop::defer's safe point, when no callback of it is on the stack
/// (ADR-0013 decision 3).
///
/// The deferred queue cannot overflow: every connection retires once, and the loop's bound is
/// sized from max_connections by the Server. Thread-affinity: loop-thread-only.
class ConnectionTable final : public IRetireSink {
public:
    /// `on_removed` runs after each destruction (the Server uses it to notice a drained shutdown).
    ConnectionTable(EventLoop& loop, std::function<void()> on_removed);

    /// Take ownership. May throw std::bad_alloc: the caller (the accept callback) is the boundary.
    Connection& insert(std::unique_ptr<Connection> connection);

    void retire(ConnectionId id) override;

    /// Live connections, including finished ones not yet destroyed.
    [[nodiscard]] std::size_t size() const noexcept {
        return connections_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return connections_.empty();
    }

    /// Visit every connection. The visitor may finish connections (they are destroyed later, so
    /// iteration is safe) but must not insert.
    void for_each(const std::function<void(Connection&)>& visit);

private:
    EventLoop& loop_;
    std::function<void()> on_removed_;
    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> connections_;
};

}  // namespace wsrv
