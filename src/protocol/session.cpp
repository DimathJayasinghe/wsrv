// SPDX-License-Identifier: Apache-2.0
#include "protocol/session.hpp"

#include "protocol/utf8.hpp"

#include <algorithm>

namespace wsrv {

namespace {

[[nodiscard]] SessionEvent make_error(CloseCode code, std::string_view reason) {
    SessionEvent event;
    event.kind = SessionEvent::Kind::ProtocolError;
    event.code = code;
    event.reason = reason;
    return event;
}

[[nodiscard]] SessionEvent make_data_event(const MessageEvent& e) {
    SessionEvent event;
    event.kind = e.kind == MessageEvent::Kind::Message ? SessionEvent::Kind::Message
                                                       : SessionEvent::Kind::Part;
    event.type = e.type;
    event.data = e.data;
    event.tail = e.tail;
    event.final = e.final;
    return event;
}

}  // namespace

void ProtocolSession::EventQueue::push(const SessionEvent& event) {
    WSRV_ASSERT(size_ < items_.size());
    items_[(head_ + size_) % items_.size()] = event;
    ++size_;
}

std::optional<SessionEvent> ProtocolSession::EventQueue::pop() {
    if (size_ == 0) {
        return std::nullopt;
    }
    const SessionEvent event = items_[head_];
    head_ = (head_ + 1) % items_.size();
    --size_;
    return event;
}

ProtocolSession::ProtocolSession(SessionConfig config, TimePoint now)
    : config_{config}, parser_{config.parser}, assembler_{config.assembler}, out_{config.outbound},
      now_{now} {}

// ---- input ------------------------------------------------------------------------------------

std::optional<SessionEvent> ProtocolSession::next_event(MutableByteView& input) {
    if (auto queued = queue_.pop()) {
        return queued;
    }
    return pump(input);
}

/// Read frames until one produces an event. Once the peer's Close has arrived, or the peer has
/// violated the protocol, the rest of the input is thrown away (FR-C5, EC-C10).
std::optional<SessionEvent> ProtocolSession::pump(MutableByteView& input) {
    while (state_ != SessionState::Closed && !close_received_ && !failed_) {
        const FrameResult<std::optional<FrameEvent>> frame = parser_.next(input);
        if (!frame) {
            return fail_connection(frame.error());
        }
        if (!frame->has_value()) {
            return std::nullopt;  // needs more bytes
        }
        if (auto event = on_frame(**frame)) {
            return event;
        }
        if (auto queued = queue_.pop()) {
            return queued;
        }
    }
    input = input.subspan(input.size());
    return queue_.pop();
}

std::optional<SessionEvent> ProtocolSession::on_frame(const FrameEvent& frame) {
    const auto step = assembler_.handle(frame);
    if (!step) {
        return fail_connection(step.error());
    }
    const std::optional<MessageEvent>& event = *step;
    if (!event) {
        return std::nullopt;
    }
    return translate(*event);
}

std::optional<SessionEvent> ProtocolSession::translate(const MessageEvent& event) {
    if (event.kind == MessageEvent::Kind::Control) {
        return on_control(event);
    }
    if (close_sent_) {
        return std::nullopt;  // we already said goodbye: the peer's data is dropped (EC-C10)
    }
    return make_data_event(event);
}

std::optional<SessionEvent> ProtocolSession::on_control(const MessageEvent& event) {
    if (event.control_opcode == Opcode::Close) {
        return on_peer_close(event.data);
    }
    if (close_sent_) {
        return std::nullopt;  // nothing may follow our Close frame
    }
    SessionEvent out;
    out.data = event.data;
    if (event.control_opcode == Opcode::Ping) {
        out_.queue_pong(event.data);  // same payload, next frame boundary (RFC 6455 section 5.5.3)
        out.kind = SessionEvent::Kind::Ping;
    } else {
        out.kind = SessionEvent::Kind::Pong;
    }
    return out;
}

/// RFC 6455 section 5.5.1 / 7.1.4: validate, remember the code and reason, echo the code (or an
/// empty Close) unless our own Close is already on its way, and stop reading.
std::optional<SessionEvent> ProtocolSession::on_peer_close(ByteView payload) {
    const FrameResult<std::optional<ClosePayload>> parsed = parse_close_payload(payload);
    if (!parsed) {
        return fail_connection(parsed.error());
    }
    close_received_ = true;
    assembler_.discard_message();  // a half-delivered message is never completed (EC-F26)
    const std::optional<ClosePayload>& close = *parsed;
    if (close) {
        peer_code_ = close->code;
        peer_reason_length_ = close->reason.size();
        std::copy(close->reason.begin(), close->reason.end(), peer_reason_.begin());
    }
    if (!close_sent_) {
        begin_close(peer_code_, {});
    }
    check_close_completion();
    return std::nullopt;
}

/// The peer broke a rule: answer with exactly one Close carrying the code, report it, ignore the
/// peer from now on (RFC 6455 section 7.1.7 "Fail the WebSocket Connection").
std::optional<SessionEvent> ProtocolSession::fail_connection(const ProtocolFailure& failure) {
    failed_ = true;
    failure_reason_ = failure.reason;
    if (!close_sent_) {
        begin_close(static_cast<std::uint16_t>(failure.code), failure.reason);
    }
    check_close_completion();
    return make_error(failure.code, failure.reason);
}

// ---- closing ----------------------------------------------------------------------------------

void ProtocolSession::begin_close(std::optional<std::uint16_t> code, std::string_view reason) {
    out_.queue_close(code, reason);
    close_sent_ = true;
    sent_code_ = code.value_or(static_cast<std::uint16_t>(CloseCode::NoStatusReceived));
    state_ = SessionState::Closing;
    assembler_.discard_message();
}

Result<void> ProtocolSession::close(CloseCode code, std::string_view reason) {
    const auto raw = static_cast<std::uint16_t>(code);
    if (!is_valid_on_wire(raw) || !is_valid_utf8(as_bytes(reason))) {
        return fail(ErrorCode::InvalidArgument, "close code or reason must not be sent");
    }
    if (state_ == SessionState::Open) {
        begin_close(raw, reason);
    }
    return {};
}

/// Closed is reported once our Close frame has been written AND the handshake is over: the peer's
/// Close arrived (clean), or we failed the connection (not clean; RFC 6455 section 7.1.7 does not
/// wait for the peer).
void ProtocolSession::check_close_completion() {
    if (state_ != SessionState::Closing || !out_.close_flushed()) {
        return;
    }
    if (close_received_) {
        emit_closed(true);
    } else if (failed_) {
        emit_closed(false);
    }
}

/// Closed.code is the peer's code if it sent one (1005 if none), else the code we sent when we
/// failed the connection, else 1006 (the transport died or the peer never answered).
void ProtocolSession::emit_closed(bool clean) {
    SessionEvent event;
    event.kind = SessionEvent::Kind::Closed;
    event.clean = clean;
    if (close_received_) {
        event.code = static_cast<CloseCode>(peer_code_.value_or(1005));
        event.reason = std::string_view{peer_reason_.data(), peer_reason_length_};
    } else if (failed_ && out_.close_flushed()) {
        event.code = static_cast<CloseCode>(sent_code_);
        event.reason = failure_reason_;
    } else {
        event.code = CloseCode::AbnormalClosure;
    }
    state_ = SessionState::Closed;
    queue_.push(event);
}

void ProtocolSession::on_transport_closed() {
    if (state_ != SessionState::Closed) {
        emit_closed(close_received_);  // the peer's Close arrived first: still a clean close
    }
}

// ---- output -----------------------------------------------------------------------------------

Result<void> ProtocolSession::send_text(std::string_view text) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    if (!is_valid_utf8(as_bytes(text))) {
        return fail(ErrorCode::InvalidUtf8, "text message is not valid UTF-8");
    }
    return out_.enqueue_message(Opcode::Text, as_bytes(text));
}

Result<void> ProtocolSession::send_binary(ByteView data) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    return out_.enqueue_message(Opcode::Binary, data);
}

Result<void> ProtocolSession::send_ping(ByteView payload) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    return out_.queue_ping(payload);
}

ByteView ProtocolSession::pending_output() {
    if (state_ == SessionState::Closed) {
        return {};
    }
    const ByteView view = out_.pending();
    if (out_.failed()) {
        give_up();
        return {};
    }
    return view;
}

void ProtocolSession::consume_output(std::size_t count) {
    if (state_ == SessionState::Closed) {
        return;
    }
    out_.consume(count);
    check_close_completion();
}

/// Out of memory while framing output: not even a Close can be written. Report and stop.
void ProtocolSession::give_up() {
    queue_.push(make_error(CloseCode::InternalError, "out of memory while writing"));
    emit_closed(false);
}

void ProtocolSession::tick(TimePoint now) {
    now_ = std::max(now_, now);
}

}  // namespace wsrv
