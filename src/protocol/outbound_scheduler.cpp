// SPDX-License-Identifier: Apache-2.0
#include "protocol/outbound_scheduler.hpp"

#include "protocol/close_payload.hpp"
#include "protocol/frame.hpp"
#include "protocol/frame_writer.hpp"

#include <algorithm>
#include <new>

namespace wsrv {

namespace {

/// One data frame plus a Pong, a Ping and a Close (each header + payload at most 2 + 125 bytes).
constexpr std::size_t wire_bound(std::size_t max_frame_payload) {
    return max_frame_payload + kMaxHeaderSize + 3 * (2 + kMaxControlPayload);
}

}  // namespace

OutboundScheduler::OutboundScheduler(OutboundConfig config)
    : config_{config}, wire_{wire_bound(config.max_frame_payload)}, data_{config.max_queued_bytes} {
    WSRV_ASSERT(config.max_frame_payload > 0);
}

Result<void> OutboundScheduler::enqueue_message(Opcode text_or_binary, ByteView payload) {
    WSRV_ASSERT(text_or_binary == Opcode::Text || text_or_binary == Opcode::Binary);
    if (close_queued_) {
        return fail(ErrorCode::Closing, "outbound: closing");
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
    if (!close_queued_) {
        fill(pong_, payload);  // replaces an unsent one (EC-C02)
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
    queued_ = 0;
    pong_.present = false;
    ping_.present = false;
    if (code.has_value()) {
        close_.length = encode_close_payload(close_.payload, static_cast<CloseCode>(*code), reason);
    }
    close_.present = true;
}

void OutboundScheduler::append_frame(Opcode opcode, bool fin, ByteView payload) {
    FrameHeader header;
    header.fin = fin;
    header.opcode = opcode;
    header.payload_length = payload.size();
    std::array<std::byte, kMaxHeaderSize> head{};
    const Result<std::size_t> n = encode_header(head, header);
    WSRV_ASSERT(n.has_value());  // lengths are bounded by construction
    // The wire buffer is sized for exactly this (wire_bound); a failure would be a logic error.
    WSRV_ASSERT(wire_.append(ByteView{head}.first(*n)).has_value());
    WSRV_ASSERT(wire_.append(payload).has_value());
}

void OutboundScheduler::append_control(Opcode opcode, Control& slot) {
    append_frame(opcode, true, ByteView{slot.payload}.first(slot.length));
    slot.present = false;
}

void OutboundScheduler::append_next_fragment() {
    const Queued& front = messages_.front();
    const std::size_t remaining = front.length - front_offset_;
    const std::size_t take = std::min(remaining, config_.max_frame_payload);
    const bool last = take == remaining;
    append_frame(
        front_offset_ == 0 ? front.opcode : Opcode::Continuation, last, data_.view().first(take));
    data_.consume(take);
    queued_ -= take;
    front_offset_ += take;
    if (last) {
        queued_ -= kMessageOverhead;
        front_offset_ = 0;
        messages_.pop_front();
    }
}

void OutboundScheduler::refill() {
    if (close_.present) {
        append_control(Opcode::Close, close_);
        close_in_wire_ = true;
        return;
    }
    if (pong_.present) {
        append_control(Opcode::Pong, pong_);
    }
    if (ping_.present) {
        append_control(Opcode::Ping, ping_);
    }
    if (!messages_.empty()) {
        append_next_fragment();
    }
}

ByteView OutboundScheduler::pending() {
    if (wire_.size() == 0) {
        refill();
    }
    return wire_.view();
}

void OutboundScheduler::consume(std::size_t count) noexcept {
    wire_.consume(count);
}

bool OutboundScheduler::has_pending() const noexcept {
    return wire_.size() > 0 || pong_.present || ping_.present || close_.present ||
           !messages_.empty();
}

}  // namespace wsrv
