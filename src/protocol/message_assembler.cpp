// SPDX-License-Identifier: Apache-2.0
#include "protocol/message_assembler.hpp"

#include <algorithm>

namespace wsrv {

MessageAssembler::MessageAssembler(AssemblerConfig config)
    : config_{config}, buffer_{config.max_message_size} {}

std::unexpected<ProtocolFailure> MessageAssembler::fail_with(CloseCode code,
                                                             std::string_view reason) {
    failed_ = true;
    failure_ = ProtocolFailure{code, reason};
    return protocol_failure(code, reason);
}

void MessageAssembler::discard_message() noexcept {
    draining_ = in_message_;  // the peer's remaining fragments of this message are still coming
    in_message_ = false;
    fragments_ = 0;
    total_ = 0;
    utf8_.reset();
    buffer_.clear();
    clear_buffer_ = false;
    final_part_sent_ = false;
    held_len_ = 0;
}

void MessageAssembler::reset() noexcept {
    discard_message();
    draining_ = false;
    failed_ = false;
    frame_ = FrameHeader{};
    frame_received_ = 0;
    control_len_ = 0;
}

MessageAssembler::StepResult MessageAssembler::handle(const FrameEvent& event) {
    if (failed_) {
        return protocol_failure(failure_.code, failure_.reason);
    }
    if (clear_buffer_) {  // the previous Message view is no longer needed
        buffer_.clear();
        clear_buffer_ = false;
    }
    switch (event.kind) {
    case FrameEvent::Kind::Header:
        return on_header(event.header);
    case FrameEvent::Kind::Payload:
        return on_payload(event);
    case FrameEvent::Kind::End:
        return on_end(event.header);
    }
    return fail_with(CloseCode::InternalError, "invalid frame event");
}

/// Sequencing, fragment-count and size checks, all before any payload byte of this frame is seen.
FrameResult<void> MessageAssembler::start_or_continue(const FrameHeader& header) {
    if (header.opcode == Opcode::Continuation) {
        if (!in_message_ && !draining_) {
            return fail_with(CloseCode::ProtocolError,
                             "continuation frame without a message in progress");
        }
    } else {
        if (in_message_ || draining_) {
            return fail_with(CloseCode::ProtocolError,
                             "new data frame during a fragmented message");
        }
        discard_message();  // fresh state for the new message
        in_message_ = true;
        type_ = header.opcode == Opcode::Text ? MessageType::Text : MessageType::Binary;
    }
    if (++fragments_ > config_.max_fragments) {
        return fail_with(CloseCode::PolicyViolation, "too many fragments in one message");
    }
    if (header.payload_length > config_.max_message_size - total_) {
        return fail_with(CloseCode::MessageTooBig, "message exceeds the size limit");
    }
    return {};
}

MessageAssembler::StepResult MessageAssembler::on_header(const FrameHeader& header) {
    frame_ = header;
    frame_received_ = 0;
    if (is_control(header.opcode)) {
        control_len_ = 0;  // control frames never touch the message state
        return std::optional<MessageEvent>{};
    }
    if (const FrameResult<void> ok = start_or_continue(header); !ok) {
        return std::unexpected{ok.error()};
    }
    return std::optional<MessageEvent>{};
}

MessageAssembler::StepResult MessageAssembler::on_payload(const FrameEvent& event) {
    const ByteView chunk = event.payload;
    if (is_control(frame_.opcode)) {
        WSRV_ASSERT(control_len_ + chunk.size() <=
                    control_.size());  // the parser caps control frames at 125
        std::copy(chunk.begin(),
                  chunk.end(),
                  control_.begin() + static_cast<std::ptrdiff_t>(control_len_));
        control_len_ += chunk.size();
        return std::optional<MessageEvent>{};
    }
    // The header check already bounds a frame's declared length, so this only fires for payload
    // that does not belong to a declared length: the inflated bytes of a compressed message
    // (ADR-0020 section 4), where the limit must apply to what the application will see.
    if (chunk.size() > config_.max_message_size - total_) {
        return fail_with(CloseCode::MessageTooBig, "message exceeds the size limit");
    }
    total_ += chunk.size();
    frame_received_ += chunk.size();
    if (draining_) {
        return std::optional<MessageEvent>{};  // discarded message: counted, never delivered
    }
    const bool last_of_message = frame_.fin && frame_received_ == frame_.payload_length;
    return config_.streaming ? stream_payload(chunk, last_of_message) : buffer_payload(chunk);
}

MessageAssembler::StepResult MessageAssembler::buffer_payload(ByteView chunk) {
    if (type_ == MessageType::Text && !utf8_.feed(chunk)) {
        return fail_with(CloseCode::InvalidPayload, "text message is not valid UTF-8");
    }
    if (const Result<void> appended = buffer_.append(chunk); !appended) {
        const bool too_big = appended.error().code == ErrorCode::BufferFull;
        return fail_with(too_big ? CloseCode::MessageTooBig : CloseCode::InternalError,
                         "cannot buffer the message");
    }
    return std::optional<MessageEvent>{};
}

MessageAssembler::StepResult MessageAssembler::stream_payload(ByteView chunk,
                                                              bool last_of_message) {
    if (type_ == MessageType::Text) {
        return stream_text_payload(chunk, last_of_message);
    }
    final_part_sent_ = final_part_sent_ || last_of_message;
    return std::optional<MessageEvent>{
        MessageEvent{MessageEvent::Kind::Part, type_, chunk, {}, last_of_message, Opcode::Ping}};
}

/// Bytes of `chunk` that, appended to the held bytes, complete the held character; the character is
/// left in join_ (held + those bytes). 0 means the chunk ends before the character does.
std::size_t MessageAssembler::complete_held_character(ByteView chunk) {
    std::copy(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(held_len_), join_.begin());
    const std::size_t room = std::min(chunk.size(), join_.size() - held_len_);
    for (std::size_t used = 1; used <= room; ++used) {
        join_[held_len_ + used - 1] = chunk[used - 1];
        if (is_valid_utf8(ByteView{join_}.first(held_len_ + used))) {
            return used;
        }
    }
    return 0;
}

/// Text parts must end on a code-point boundary: bytes of an unfinished character are held back (at
/// most 3) and delivered with the next part. A character that spans chunks is completed in the
/// 4-byte join_ array and delivered as `data`, the rest of the chunk follows as `tail`; nothing of
/// the chunk is copied, so memory stays O(1) however large the peer's reads are.
MessageAssembler::StepResult MessageAssembler::stream_text_payload(ByteView chunk,
                                                                   bool last_of_message) {
    if (!utf8_.feed(chunk)) {
        return fail_with(CloseCode::InvalidPayload, "text message is not valid UTF-8");
    }
    const std::size_t pending = utf8_.pending_bytes();
    if (last_of_message && pending != 0) {
        return fail_with(CloseCode::InvalidPayload, "text message ends inside a character");
    }
    ByteView completed;
    ByteView rest = chunk;
    if (held_len_ > 0) {
        const std::size_t used = complete_held_character(chunk);
        if (used == 0) {  // still inside the character: hold everything (pending == held + chunk)
            std::copy(
                chunk.begin(), chunk.end(), held_.begin() + static_cast<std::ptrdiff_t>(held_len_));
            held_len_ = pending;
            return std::optional<MessageEvent>{};
        }
        completed = ByteView{join_}.first(held_len_ + used);
        rest = chunk.subspan(used);
    }
    const ByteView deliver = rest.first(rest.size() - pending);
    std::copy(deliver.end(), rest.end(), held_.begin());
    held_len_ = pending;
    final_part_sent_ = final_part_sent_ || last_of_message;
    if (completed.empty() && deliver.empty() && !last_of_message) {
        return std::optional<MessageEvent>{};  // nothing to hand on yet
    }
    const bool lead_is_completed = !completed.empty();
    return std::optional<MessageEvent>{MessageEvent{MessageEvent::Kind::Part,
                                                    type_,
                                                    lead_is_completed ? completed : deliver,
                                                    lead_is_completed ? deliver : ByteView{},
                                                    last_of_message,
                                                    Opcode::Ping}};
}

MessageAssembler::StepResult MessageAssembler::on_end(const FrameHeader& header) {
    if (is_control(header.opcode)) {
        return std::optional<MessageEvent>{MessageEvent{MessageEvent::Kind::Control,
                                                        MessageType::Text,
                                                        ByteView{control_}.first(control_len_),
                                                        {},
                                                        true,
                                                        header.opcode}};
    }
    if (!header.fin) {
        return std::optional<MessageEvent>{};  // more fragments to come
    }
    in_message_ = false;
    if (draining_) {
        draining_ = false;  // the discarded message is complete; the next frame starts a new one
        return std::optional<MessageEvent>{};
    }
    if (type_ == MessageType::Text && !utf8_.at_boundary()) {
        return fail_with(CloseCode::InvalidPayload, "text message ends inside a character");
    }
    if (config_.streaming) {
        if (final_part_sent_) {
            return std::optional<MessageEvent>{};
        }
        final_part_sent_ = true;  // a zero-length final frame: close the stream with an empty part
        return std::optional<MessageEvent>{
            MessageEvent{MessageEvent::Kind::Part, type_, {}, {}, true, Opcode::Ping}};
    }
    clear_buffer_ = true;
    return std::optional<MessageEvent>{
        MessageEvent{MessageEvent::Kind::Message, type_, buffer_.view(), {}, true, Opcode::Ping}};
}

}  // namespace wsrv
