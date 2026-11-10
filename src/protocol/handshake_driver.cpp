// SPDX-License-Identifier: Apache-2.0
#include "protocol/handshake_driver.hpp"

#include <utility>

namespace wsrv {

HandshakeDriver::HandshakeDriver(const HandshakeNegotiator& negotiator, const HttpLimits& limits)
    : negotiator_{negotiator}, parser_{limits} {}

HandshakeOutcome HandshakeDriver::feed(ByteView chunk) {
    if (finished_) {
        HandshakeOutcome ignored;
        ignored.step = HandshakeStep::Finished;
        return ignored;
    }
    const HttpResult<ParseProgress> progress = parser_.feed(chunk);
    if (!progress) {
        return reject(progress.error(), chunk.size());
    }
    if (!progress->complete) {
        HandshakeOutcome waiting;
        waiting.step = HandshakeStep::NeedMore;
        waiting.consumed = progress->consumed;
        return waiting;
    }
    return decide(progress->consumed);
}

HandshakeOutcome HandshakeDriver::decide(std::size_t consumed) {
    finished_ = true;
    const HandshakeDecision decision = negotiator_.negotiate(parser_.request());
    HandshakeOutcome outcome;
    outcome.consumed = consumed;
    outcome.response = render_response(decision);
    if (const auto* accepted = std::get_if<HandshakeAccept>(&decision)) {
        outcome.step = HandshakeStep::Accepted;
        outcome.subprotocol = accepted->subprotocol;
        outcome.annotations = accepted->annotations;
        outcome.accept_key = accepted->accept_key;
        outcome.deflate = accepted->deflate;
    } else {
        outcome.step = HandshakeStep::Rejected;
    }
    return outcome;
}

/// A request that never parsed: answer with the parser's status and close. The whole chunk counts
/// as consumed because the connection is going away and nothing in it can be trusted.
HandshakeOutcome HandshakeDriver::reject(const HttpError& error, std::size_t consumed) {
    finished_ = true;
    HandshakeOutcome outcome;
    outcome.step = HandshakeStep::Rejected;
    outcome.consumed = consumed;
    outcome.response = render_response(HandshakeDecision{HandshakeReject{error.status, {}, false}});
    return outcome;
}

}  // namespace wsrv
