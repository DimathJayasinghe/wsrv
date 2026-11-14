// SPDX-License-Identifier: Apache-2.0
#include "server/connection_table.hpp"

#include "base/assert.hpp"

#include <utility>

namespace wsrv {

ConnectionTable::ConnectionTable(EventLoop& loop, std::function<void(const Endpoint&)> on_removed)
    : loop_{loop}, on_removed_{std::move(on_removed)} {}

Connection& ConnectionTable::insert(std::unique_ptr<Connection> connection) {
    WSRV_ASSERT(connection != nullptr);
    Connection& ref = *connection;
    const bool added = connections_.emplace(ref.id(), std::move(connection)).second;
    WSRV_ASSERT(added);  // ids come from a counter and are never reused
    return ref;
}

void ConnectionTable::retire(ConnectionId id) {
    // Erase later, not now: the connection is still executing the call that retired it.
    const Result<void> queued = loop_.defer([this, id] {
        const auto found = connections_.find(id);
        if (found == connections_.end()) {
            return;  // retired twice: nothing left to give back
        }
        const Endpoint peer = found->second->peer();  // copied: the connection is destroyed next
        connections_.erase(found);
        if (on_removed_) {
            on_removed_(peer);
        }
    });
    WSRV_ASSERT(queued.has_value());  // sized from max_connections: one retire per connection
}

void ConnectionTable::run(ConnectionId id, const SessionCommand& command) {
    const auto found = connections_.find(id);
    if (found != connections_.end()) {
        found->second->apply(command);
    }
}

void ConnectionTable::decide(ConnectionId id, const HandshakeVerdict& verdict) {
    const auto found = connections_.find(id);
    if (found != connections_.end()) {
        found->second->decide(verdict);
    }
}

void ConnectionTable::for_each(const std::function<void(Connection&)>& visit) {
    for (auto& entry : connections_) {
        visit(*entry.second);
    }
}

}  // namespace wsrv
