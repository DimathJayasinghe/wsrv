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
    /// The broker commands (IPC 0.3, ADR-0028). A subscription needs a connection the application
    /// was told about (NO_SUCH_CONN otherwise); a publish needs only a request id. A broker bound
    /// is WSRV_ERR_LIMIT, not backpressure: waiting does not help until someone leaves.
    [[nodiscard]] ipc::Status execute(const ipc::TopicCommand& command);
    [[nodiscard]] ipc::Status execute(const ipc::Publish& command);
    /// Answers to a handshake question (ADR-0018). Not checked against the open set: the question
    /// is about a connection that does not exist yet. The backend knows which questions are open
    /// (it also learns when the server gives up on one), so a late or repeated answer comes back
    /// as NO_SUCH_CONN from there, without this class keeping a set that could only grow.
    [[nodiscard]] ipc::Status execute(const ipc::HandshakeAccept& command);
    [[nodiscard]] ipc::Status execute(const ipc::HandshakeReject& command);

    /// Connections the application knows to be open.
    [[nodiscard]] std::size_t open_count() const noexcept {
        return open_.size();
    }

    /// The application is gone. Every connection it knew becomes STALE: nobody can serve it, its
    /// further events are dropped, and commands for it are refused. This is the ONLY place where
    /// events are discarded, and only events of connections whose application no longer exists.
    void detach();

    /// Close every stale connection with `code` and `reason`. A close its loop accepts, or a
    /// connection that is already gone, is done and not asked again; a close the loop REFUSES
    /// because its task queue is full (BufferFull) or it is out of memory leaves the connection
    /// open, so it stays to be retried: call again later (ADR-0034). Stale entries are forgotten as
    /// their Close events go by in next_event().
    /// @return how many closes were refused and are still to do (0 = nothing left to retry).
    [[nodiscard]] std::size_t close_stale(std::uint16_t code, std::string_view reason);

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
    std::unordered_set<ConnectionId> closing_;  ///< stale, and its close was accepted or is moot
};

}  // namespace wsrv::wsrvd
