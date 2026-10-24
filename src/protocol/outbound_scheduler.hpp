// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/assert.hpp"
#include "base/byte_buffer.hpp"
#include "base/bytes.hpp"
#include "base/error.hpp"
#include "protocol/opcode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>

namespace wsrv {

struct OutboundConfig {
    /// Largest payload of one outgoing frame; longer messages are fragmented (FRAGMENTATION.md
    /// section 2.1, EC-F31). Small enough that a Pong or Close never waits long behind a data
    /// frame.
    std::size_t max_frame_payload = std::size_t{64} * 1024;
    /// Bound on queued, not yet framed data: payload bytes plus kMessageOverhead per message (so
    /// empty messages cannot be queued without limit). Over it: Backpressure (EC-N09).
    std::size_t max_queued_bytes = 1U << 20;
};

/// Cost charged to the queue bound for every queued message on top of its payload.
inline constexpr std::size_t kMessageOverhead = 16;

/// Decides WHAT goes on the wire next (docs/FRAGMENTATION.md section 2.3). Data messages wait
/// unframed; at every frame boundary the scheduler emits, in this order: the Close frame (nothing
/// after it, RFC 6455 section 5.5.1), else the pending Pong, the pending Ping, then the next
/// fragment of the oldest data message. A frame is never interleaved with another: control frames
/// wait for the data frame in flight, then go first (FR-F10, EC-F32, EC-C01).
///
/// Every queue is bounded: data by max_queued_bytes, Pong / Ping / Close by one slot each (a newer
/// Pong replaces an unsent one, which RFC 6455 section 5.5.3 allows, EC-C02). Frames are unmasked
/// (server role). The wire buffer holds at most one data frame plus the control frames.
///
/// Thread-affinity: loop-thread-only (not synchronised).
class OutboundScheduler {
public:
    explicit OutboundScheduler(OutboundConfig config);

    /// Queue one complete Text or Binary message.
    /// @return Error{Closing} after queue_close(); Error{Backpressure} if the queue is too full
    /// right now; Error{BufferFull} if the message could never fit. Nothing is queued on error.
    [[nodiscard]] Result<void> enqueue_message(Opcode text_or_binary, ByteView payload);

    /// Queue a Ping. One may wait at a time: Error{Backpressure} if one is unsent,
    /// Error{InvalidArgument} if payload > 125 bytes, Error{Closing} after queue_close().
    [[nodiscard]] Result<void> queue_ping(ByteView payload);

    /// Queue a Pong, replacing an unsent one. Precondition: payload <= 125 bytes (the parser
    /// guarantees it for a Ping's payload). Ignored after queue_close().
    void queue_pong(ByteView payload);

    /// Queue the Close frame: `code` (must be valid on the wire) + `reason` (valid UTF-8; cut to
    /// 123 bytes), or an empty Close if `code` is empty. Drops everything unsent (data, Ping,
    /// Pong); a frame already in the wire buffer is still finished. Idempotent.
    void queue_close(std::optional<std::uint16_t> code, std::string_view reason);

    /// Bytes to write now (refills at a frame boundary). Empty means nothing to write.
    /// Invalidated by consume() and any queue_* call.
    [[nodiscard]] ByteView pending();

    /// The transport accepted `count` bytes of pending(). Precondition: count <= pending().size().
    void consume(std::size_t count) noexcept;

    [[nodiscard]] bool close_queued() const noexcept {
        return close_queued_;
    }
    /// True once the Close frame has been completely consumed.
    [[nodiscard]] bool close_flushed() const noexcept {
        return close_in_wire_ && wire_.size() == 0;
    }
    [[nodiscard]] bool has_pending() const noexcept;

    /// True once the wire buffer could not be allocated (out of memory). The scheduler then
    /// produces nothing more and the session must give up on the connection: there is no memory
    /// left to even frame a Close.
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }

private:
    struct Control {
        std::array<std::byte, kMaxControlPayload> payload{};
        std::size_t length = 0;
        bool present = false;
    };
    struct Queued {
        Opcode opcode;
        std::size_t length;
    };

    void refill();
    [[nodiscard]] bool append_frame(Opcode opcode, bool fin, ByteView payload);
    void append_control(Opcode opcode, Control& slot);
    void append_next_fragment();
    static void fill(Control& slot, ByteView payload) noexcept;

    OutboundConfig config_;
    ByteBuffer wire_;  // framed bytes ready to write
    ByteBuffer data_;  // payloads of queued messages, back to back
    std::deque<Queued> messages_;
    std::size_t front_offset_ = 0;  // bytes of messages_.front() already framed
    std::size_t queued_ = 0;        // charged against max_queued_bytes
    Control pong_;
    Control ping_;
    Control close_;
    bool close_queued_ = false;
    bool close_in_wire_ = false;
    bool failed_ = false;
};

}  // namespace wsrv
