// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "http/request_parser.hpp"
#include "protocol/handshake.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace wsrv {

enum class HandshakeStep : std::uint8_t {
    NeedMore,  ///< the request head is incomplete; feed more bytes
    Accepted,  ///< upgrade accepted: write `response`, then the WebSocket session starts
    Rejected,  ///< refused: write `response` (if any) and close the connection
    Finished,  ///< a decision was already made; the input was ignored
};

/// What one HandshakeDriver::feed() produced.
struct HandshakeOutcome {
    HandshakeStep step = HandshakeStep::NeedMore;
    /// Bytes of the chunk that belong to the HTTP request head. After Accepted, the rest of the
    /// chunk is the start of the first WebSocket frame and belongs to the caller (EC-H19, FR-H8).
    std::size_t consumed = 0;
    /// The exact bytes to send (Accepted, Rejected); empty otherwise.
    std::string response;
    /// Accepted only: facts for the application, never sent to the peer.
    std::string subprotocol;
    std::map<std::string, std::string> annotations;
};

/// The opening handshake as a pure function of bytes (ADR-0002): feeds the strict HTTP parser,
/// asks the negotiator for a decision when the head is complete, and renders the response. It does
/// no I/O and knows no clock; the connection owns the socket and the handshake timeout (T2).
///
/// Every refusal, including a malformed or oversize request, becomes an HTTP response followed by
/// close, never a crash (FR-H9). The decision is made once; later input is ignored (Finished).
/// Thread-affinity: loop-thread-only. The negotiator must outlive the driver.
class HandshakeDriver {
public:
    HandshakeDriver(const HandshakeNegotiator& negotiator, const HttpLimits& limits);

    [[nodiscard]] HandshakeOutcome feed(ByteView chunk);

private:
    [[nodiscard]] HandshakeOutcome decide(std::size_t consumed);
    [[nodiscard]] HandshakeOutcome reject(const HttpError& error, std::size_t consumed);

    const HandshakeNegotiator& negotiator_;
    HttpRequestParser parser_;
    bool finished_ = false;
};

}  // namespace wsrv
