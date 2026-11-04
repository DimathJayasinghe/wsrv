// SPDX-License-Identifier: Apache-2.0
#include "protocol/outbound_scheduler.hpp"

#include "protocol/close_payload.hpp"
#include "protocol/frame.hpp"
#include "protocol/frame_writer.hpp"

#include <algorithm>
#include <new>

namespace wsrv {

namespace {

/// The wire buffer holds exactly one frame: a data frame (header + payload) or a control frame
/// (2 + at most 125 bytes).
constexpr std::size_t wire_bound(std::size_t max_frame_payload) {
    return std::max(max_frame_payload + kMaxHeaderSize, 2 + kMaxControlPayload);
}

}  // namespace

OutboundScheduler::OutboundScheduler(OutboundConfig config)
    : config_{config}, wire_{wire_bound(config.max_frame_payload)}, data_{config.max_queued_bytes} {
    WSRV_ASSERT(config.max_frame_payload > 0);
    WSRV_ASSERT(config.max_frame_payload < (std::size_t{1} << 40));  // keeps wire_bound() exact
}

Result<void> OutboundScheduler::enqueue_message(Opcode text_or_binary, ByteView payload) {
    const bool is_data_opcode = text_or_binary == Opcode::Text || text_or_binary == Opcode::Binary;
    WSRV_ASSERT(is_data_opcode);
    if (close_queued_) {
        return fail(ErrorCode::Closing, "outbound: closing");
    }
    if (stream_open_) {
        return fail(ErrorCode::MessageInProgress, "outbound: a streamed message is open");
    }
    const std::size_t limit = config_.max_queued_bytes;
    if (limit < kMessageOverhead || payload.size() > limit - kMessageOverhead) {
        return fail(ErrorCode::BufferFull, "outbound: message can never fit the queue");
    }
    const std::size_t cost = payload.size() + kMessageOverhead;
    if (cost > limit - queued_) {
        return fail(ErrorCode::Backpressure, "outbound: queue full");
    }
    try {
        messages_.push_back(Queued{text_or_binary, payload.size()});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "outbound: out of memory");
    }
    if (const Result<void> stored = data_.append(payload); !stored) {
        messages_.pop_back();
        return stored;
    }
    queued_ += cost;
    return {};
}

Result<void> OutboundScheduler::begin_stream(Opcode text_or_binary) {
    const bool is_data_opcode = text_or_binary == Opcode::Text || text_or_binary == Opcode::Binary;
    WSRV_ASSERT(is_data_opcode);
    if (close_queued_) {
        return fail(ErrorCode::Closing, "outbound: closing");
    }
    if (stream_open_) {
        return fail(ErrorCode::MessageInProgress, "outbound: a streamed message is open");
    }
    if (kMessageOverhead > config_.max_queued_bytes - queued_) {  // queued_ never exceeds the limit
        return fail(ErrorCode::Backpressure, "outbound: queue full");
    }
    try {
        messages_.push_back(Queued{text_or_binary, 0, true});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "outbound: out of memory");
    }
    queued_ += kMessageOverhead;
    stream_open_ = true;
    return {};
}

Result<void> OutboundScheduler::append_stream(ByteView bytes) {
    if (close_queued_) {
        return fail(ErrorCode::Closing, "outbound: closing");
    }
    if (!stream_open_) {
        return fail(ErrorCode::InvalidArgument, "outbound: no streamed message is open");
    }
    const std::size_t limit = config_.max_queued_bytes;
    if (bytes.size() > limit - kMessageOverhead) {
        return fail(ErrorCode::BufferFull, "outbound: piece can never fit the queue");
    }
    if (bytes.size() > limit - queued_) {
        return fail(ErrorCode::Backpressure, "outbound: queue full");
    }
    if (const Result<void> stored = data_.append(bytes); !stored) {
        return stored;
    }
    messages_.back().length += bytes.size();
    queued_ += bytes.size();
    return {};
}

Result<void> OutboundScheduler::end_stream() {
    if (close_queued_) {
        return fail(ErrorCode::Closing, "outbound: closing");
    }
    if (!stream_open_) {
        return fail(ErrorCode::InvalidArgument, "outbound: no streamed message is open");
    }
    messages_.back().open = false;
    stream_open_ = false;
    return {};
}

void OutboundScheduler::fill(Control& slot, ByteView payload) noexcept {
    WSRV_ASSERT(payload.size() <= kMaxControlPayload);
    std::copy(payload.begin(), payload.end(), slot.payload.begin());
    slot.length = payload.size();
    slot.present = true;
}

Result<void> OutboundScheduler::queue_ping(ByteView payload) {
    if (close_queued_) {
        return fail(ErrorCode::Closing, "outbound: closing");
    }
    if (payload.size() > kMaxControlPayload) {
        return fail(ErrorCode::InvalidArgument, "ping payload over 125 bytes");
    }
    if (ping_.present) {
        return fail(ErrorCode::Backpressure, "a ping is already waiting");
    }
    fill(ping_, payload);
    return {};
}

void OutboundScheduler::queue_pong(ByteView payload) {
    if (close_queued_) {
        return;
    }
    if (pongs_.size() < kMaxQueuedPongs) {
        bool room = true;
        try {
            pongs_.emplace_back();
        } catch (const std::bad_alloc&) {
            room = false;  // no memory for another entry: coalesce into the last one, as below
        }
        if (room) {
            fill(pongs_.back(), payload);
            return;
        }
    }
    if (!pongs_.empty()) {
        fill(pongs_.back(), payload);  // coalesce: the most recent Ping wins (EC-C02)
    }
}

/// The oldest Pong becomes the next frame; it stays queued if the wire buffer is out of memory.
void OutboundScheduler::append_pong() {
    Control& oldest = pongs_.front();
    if (append_frame(Opcode::Pong, true, ByteView{oldest.payload}.first(oldest.length))) {
        pongs_.erase(pongs_.begin());
    }
}

void OutboundScheduler::queue_close(std::optional<std::uint16_t> code, std::string_view reason) {
    if (close_queued_) {
        return;
    }
    close_queued_ = true;
    data_.clear();
    messages_.clear();
    front_offset_ = 0;
    front_started_ = false;
    stream_open_ = false;
    queued_ = 0;
    pongs_.clear();
    ping_.present = false;
    if (code.has_value()) {
        close_.length = encode_close_payload(close_.payload, static_cast<CloseCode>(*code), reason);
    }
    close_.present = true;
}

/// The wire buffer is sized for exactly this (wire_bound), so the only way an append can fail is
/// the allocator running dry. Then the half-written frame is dropped and the scheduler is failed.
bool OutboundScheduler::append_frame(Opcode opcode, bool fin, ByteView payload) {
    FrameHeader header;
    header.fin = fin;
    header.opcode = opcode;
    header.payload_length = payload.size();
    std::array<std::byte, kMaxHeaderSize> head{};
    const Result<std::size_t> n = encode_header(head, header);
    WSRV_ASSERT(n.has_value());  // lengths are bounded by construction
    const Result<void> wrote_head = wire_.append(ByteView{head}.first(*n));
    const Result<void> wrote_payload = wrote_head ? wire_.append(payload) : wrote_head;
    if (!wrote_payload) {
        WSRV_ASSERT(wrote_payload.error().code == ErrorCode::ResourceExhausted);
        wire_.clear();
        failed_ = true;
        return false;
    }
    return true;
}

void OutboundScheduler::append_control(Opcode opcode, Control& slot) {
    if (append_frame(opcode, true, ByteView{slot.payload}.first(slot.length))) {
        slot.present = false;
    }
}

bool OutboundScheduler::data_ready() const noexcept {
    if (messages_.empty()) {
        return false;
    }
    const Queued& front = messages_.front();
    return !front.open || front.length > front_offset_;  // an open message waits for its bytes
}

/// One frame of the front message: what is available, at most max_frame_payload. It is the last
/// frame only if the message has ended and nothing remains. A message that ended right after a
/// frame went out has nothing left but still needs its FIN: that is the empty final Continuation.
void OutboundScheduler::append_next_fragment() {
    const Queued& front = messages_.front();
    const std::size_t remaining = front.length - front_offset_;
    const std::size_t take = std::min(remaining, config_.max_frame_payload);
    const bool last = !front.open && take == remaining;
    if (!append_frame(
            front_started_ ? Opcode::Continuation : front.opcode, last, data_.view().first(take))) {
        return;
    }
    front_started_ = true;
    data_.consume(take);
    queued_ -= take;
    front_offset_ += take;
    if (last) {
        queued_ -= kMessageOverhead;
        front_offset_ = 0;
        front_started_ = false;
        messages_.pop_front();
    }
}

/// Exactly ONE frame per refill, chosen by priority (Close, Pong, Ping, next data fragment), so a
/// control frame queued while any frame is partly written waits for that frame only (EC-F32).
void OutboundScheduler::refill() {
    if (close_.present) {
        append_control(Opcode::Close, close_);
        close_in_wire_ = !failed_;
    } else if (!pongs_.empty()) {
        append_pong();
    } else if (ping_.present) {
        append_control(Opcode::Ping, ping_);
    } else if (data_ready()) {
        append_next_fragment();
    }
}

ByteView OutboundScheduler::pending() {
    if (failed_) {
        return {};
    }
    if (wire_.size() == 0) {
        refill();
    }
    return wire_.view();
}

void OutboundScheduler::consume(std::size_t count) noexcept {
    wire_.consume(count);
}

bool OutboundScheduler::has_pending() const noexcept {
    return !failed_ &&
           (wire_.size() > 0 || !pongs_.empty() || ping_.present || close_.present || data_ready());
}

}  // namespace wsrv
