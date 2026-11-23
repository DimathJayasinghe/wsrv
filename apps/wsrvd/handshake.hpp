// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ipc/message.hpp"

namespace wsrv::wsrvd {

/// The largest frame an application may send before it has been accepted: type + flags (2) and
/// the fixed part of HELLO (3 x u16 = 6) plus the longest token. Until HELLO is accepted the frame
/// decoder is bounded to this, not to ipc_max_frame, so an unauthenticated peer cannot make the
/// daemon gather a large body (ADR-0017 section 4a).
inline constexpr std::size_t kHelloMaxFrame = 2 + 6 + ipc::kMaxTokenSize;

/// The first exchange on an IPC connection (ADR-0017 sections 2 and 4a), without any I/O: feed it
/// the first decoded message, send back the reply.
///
/// Every refusal is the same message, STATUS with request id 0 and WSRV_ERR_INVALID_ARGUMENT,
/// whether the first frame was not a HELLO, the major version differs or the token is wrong.
/// Telling the peer WHICH check failed would let it probe the token apart from the version.
/// Thread-affinity: loop-thread-only (holds no shared state).
class Handshake {
public:
    struct Outcome {
        bool accepted = false;
        ipc::Message reply;  ///< HELLO_OK when accepted, else the refusing STATUS (then close)
        std::uint16_t peer_minor = 0;  ///< the minor the application said in HELLO (when accepted)
    };

    /// @param token the shared secret; empty means "no token required" (then only the peer uid
    ///        check protects the socket, which the daemon logs at startup).
    /// @param max_frame the largest frame the daemon will accept after HELLO, told to the peer.
    /// @param asks_questions the daemon asks the application about each upgrade (ADR-0018), so an
    ///        application that speaks an older minor version, and cannot read a Handshake event,
    ///        is refused: it would leave every browser waiting for a 503.
    Handshake(std::string token, std::uint32_t max_frame, bool asks_questions = false);

    [[nodiscard]] Outcome on_first_message(const ipc::Message& first) const;

private:
    std::string token_;
    std::uint32_t max_frame_;
    bool asks_questions_;
};

}  // namespace wsrv::wsrvd
