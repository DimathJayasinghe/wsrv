// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "ipc/message.hpp"
#include "wsrvd/backend.hpp"

namespace wsrv::wsrvd {

/// The daemon's rules about WHAT the application sees and WHAT its commands may do, with no
/// sockets in sight (so they are tested without any).
///
/// - It remembers which connections the application has been told about (Open seen, Close not
///   yet). A command for any other id is answered NO_SUCH_CONN without calling the backend:
///   connection ids are never reused, so an id that is not in the set is stale or invented.
/// - It answers every command with a STATUS carrying the same code the C API would return
///   (to_c_status), and refuses request id 0, which is reserved for the handshake (ADR-0017 4a).
///
/// The set is bounded by the server's connection limit, because an entry exists only between an
/// Open and its Close.
/// Thread-affinity: loop-thread-only.
class Relay {
public:
    explicit Relay(IBackend& backend) noexcept : backend_{backend} {}

    /// The next event for the application, or nothing when the backend has none. Updates the set of
    /// open connections as events go by.
    [[nodiscard]] std::optional<HostEvent> next_event();

    [[nodiscard]] ipc::Status execute(const ipc::SendData& command);
    [[nodiscard]] ipc::Status execute(const ipc::CloseCommand& command);

    /// Connections the application knows to be open.
    [[nodiscard]] std::size_t open_count() const noexcept {
        return open_.size();
    }

    /// The application is gone. Every connection it knew becomes STALE: nobody can serve it, its
    /// further events are dropped, and commands for it are refused. This is the ONLY place where
    /// events are discarded, and only events of connections whose application no longer exists.
    void detach();

    /// Close every stale connection with `code` and `reason`; failures mean it is already gone.
    /// Stale entries are forgotten as their Close events go by in next_event().
    void close_stale(std::uint16_t code, std::string_view reason);

    [[nodiscard]] std::size_t stale_count() const noexcept {
        return stale_.size();
    }

private:
    /// @return the status to answer with when the command may not run, nothing when it may.
    [[nodiscard]] std::optional<std::uint8_t> refuse(std::uint32_t request,
                                                     std::uint64_t conn) const noexcept;

    IBackend& backend_;
    std::unordered_set<ConnectionId> open_;
    std::unordered_set<ConnectionId> stale_;  ///< known to a previous application, now unserved
};

}  // namespace wsrv::wsrvd
