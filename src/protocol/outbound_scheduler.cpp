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

void OutboundScheduler::append_next_fragment() {
    const Queued& front = messages_.front();
    const std::size_t remaining = front.length - front_offset_;
    const std::size_t take = std::min(remaining, config_.max_frame_payload);
    const bool last = take == remaining;
    if (!append_frame(front_offset_ == 0 ? front.opcode : Opcode::Continuation,
                      last,
                      data_.view().first(take))) {
        return;
    }
    data_.consume(take);
    queued_ -= take;
    front_offset_ += take;
    if (last) {
        queued_ -= kMessageOverhead;
        front_offset_ = 0;
        messages_.pop_front();
    }
}

/// Exactly ONE frame per refill, chosen by priority (Close, Pong, Ping, next data fragment), so a
/// control frame queued while any frame is partly written waits for that frame only (EC-F32).
void OutboundScheduler::refill() {
    if (close_.present) {
        append_control(Opcode::Close, close_);
        close_in_wire_ = !failed_;
    } else if (pong_.present) {
        append_control(Opcode::Pong, pong_);
    } else if (ping_.present) {
        append_control(Opcode::Ping, ping_);
    } else if (!messages_.empty()) {
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
    return !failed_ && (wire_.size() > 0 || pong_.present || ping_.present || close_.present ||
                        !messages_.empty());
}

}  // namespace wsrv
