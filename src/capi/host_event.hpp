// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "server/handler.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wsrv {

/// What happened on a connection, as the host (a binding) sees it.
enum class HostEventType : std::uint8_t {
    Open,    ///< data = the peer address, "host:port" (UTF-8)
    Text,    ///< data = a complete message, valid UTF-8 (FR-F7)
    Binary,  ///< data = a complete message
    Close,   ///< data = the close reason (at most kMaxCloseReason bytes); code and clean are set
    Error,   ///< data = the error name, a short literal; best effort, may be dropped
    /// A valid upgrade waits for the host's decision (ADR-0018): `conn` is the id the connection
    /// will have if accepted, `data` is the blob of capi/handshake_blob.hpp. Not followed by a
    /// Close: only an accepted upgrade becomes a connection (and then starts with Open).
    Handshake
};

/// One event, owned by value so it can leave the loop thread: the loops copy into it, the host
/// thread reads it. For one connection the order is always Open, then messages, then Close.
struct HostEvent {
    HostEventType type = HostEventType::Open;
    ConnectionId conn = 0;
    std::vector<std::byte> data;
    std::uint16_t close_code = 0;  ///< Close only
    bool clean = false;            ///< Close only: the closing handshake completed
};

}  // namespace wsrv
