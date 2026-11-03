// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

#include "capi/host_event.hpp"

namespace wsrv::wsrvd {

/// What the daemon needs from the WebSocket server, and nothing more. The daemon talks to this
/// interface, not to EmbeddedServer, so its rules (backpressure, command checks, disconnect
/// policy) are tested with a fake and the real server is wired in only by the composition root.
///
/// Thread-affinity: every call is made on the daemon's loop thread. The implementation behind it
/// may be shared with other threads (EmbeddedServer is), but the daemon never calls from elsewhere.
class IBackend {
public:
    IBackend() = default;
    IBackend(const IBackend&) = delete;
    IBackend& operator=(const IBackend&) = delete;
    IBackend(IBackend&&) = delete;
    IBackend& operator=(IBackend&&) = delete;
    virtual ~IBackend() = default;

    /// The oldest pending event, or nothing. Never blocks.
    [[nodiscard]] virtual std::optional<HostEvent> poll() = 0;

    /// Readable exactly while poll() would return an event (EventQueue::event_fd). The daemon
    /// watches it on its loop; it is edge-triggered there, so a reader that stops before the queue
    /// is empty must come back on its own (see Pump).
    [[nodiscard]] virtual int event_fd() const noexcept = 0;

    /// Commands by connection id. Accepted, not delivered (ADR-0016 section 5). A connection that
    /// is gone or unknown answers ErrorCode::Closing.
    [[nodiscard]] virtual Result<void> send_text(ConnectionId conn, ByteView text) = 0;
    [[nodiscard]] virtual Result<void> send_binary(ConnectionId conn, ByteView data) = 0;
    [[nodiscard]] virtual Result<void>
    close(ConnectionId conn, std::uint16_t code, std::string_view reason) = 0;
};

}  // namespace wsrv::wsrvd
