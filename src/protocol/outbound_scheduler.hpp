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
#include <vector>

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

/// Most Pongs waiting to be sent at once: 16 x (125 + 2) bytes at worst, per connection, and only
/// while a peer is pinging faster than the socket drains.
inline constexpr std::size_t kMaxQueuedPongs = 16;

/// Decides WHAT goes on the wire next (docs/FRAGMENTATION.md section 2.3). Data messages wait
/// unframed; at every frame boundary the scheduler emits, in this order: the Close frame (nothing
/// after it, RFC 6455 section 5.5.1), else the pending Pong, the pending Ping, then the next
/// fragment of the oldest data message. Exactly one frame is framed at a time, so a frame is never
/// interleaved with another: control frames wait for the frame in flight (and only that one), then
/// go first (FR-F10, EC-F32, EC-C01).
///
/// Every queue is bounded: data by max_queued_bytes, Ping / Close by one slot each, Pongs by
/// kMaxQueuedPongs. A Ping burst gets one Pong per Ping while they fit (what peers and conformance
/// suites expect, Autobahn 2.10); beyond that the newest Pong replaces the last queued one, which
/// RFC 6455 section 5.5.3 allows ("a Pong for only the most recent Ping", EC-C02). Frames are
/// unmasked (server role). The wire buffer holds exactly one frame.
///
/// Thread-affinity: loop-thread-only (not synchronised).
class OutboundScheduler {
public:
    explicit OutboundScheduler(OutboundConfig config);

    /// Queue one complete Text or Binary message. `compressed` marks a payload that is already
    /// permessage-deflate output (RFC 7692 section 6): its first frame, and only that one, carries
    /// RSV1 (EC-D02).
    /// @return Error{Closing} after queue_close(); Error{Backpressure} if the queue is too full
    /// right now; Error{BufferFull} if the message could never fit. Nothing is queued on error.
    [[nodiscard]] Result<void>
    enqueue_message(Opcode text_or_binary, ByteView payload, bool compressed = false);
    /// The error enqueue_message() would give for a payload of `size` bytes right now, or success.
    /// Lets a caller that changes state to produce the payload (a compressor) find out first.
    [[nodiscard]] Result<void> can_enqueue(std::size_t size) const;

    /// Streaming send (ADR-0015): open a message whose bytes arrive later. The message sits at the
    /// end of the data queue and is framed as its bytes become available: a non-final frame per
    /// chunk (the first with the opcode, then Continuations), and an empty final Continuation
    /// frame once it is ended. While it is open no other data message may be queued.
    /// @return Error{MessageInProgress}, Error{Closing}, Error{Backpressure} (not even the
    /// per-message overhead fits), Error{ResourceExhausted}.
    [[nodiscard]] Result<void> begin_stream(Opcode text_or_binary);
    /// Add bytes to the open message. @return Error{InvalidArgument} if none is open,
    /// Error{Closing}, Error{Backpressure} if the bytes do not fit the queue right now (nothing is
    /// queued: write again after the queue drained), Error{BufferFull} if they could never fit.
    [[nodiscard]] Result<void> append_stream(ByteView bytes);
    /// Finish the open message. @return Error{InvalidArgument} if none is open, Error{Closing}.
    [[nodiscard]] Result<void> end_stream();
    [[nodiscard]] bool stream_open() const noexcept {
        return stream_open_;
    }
    /// Bytes charged against max_queued_bytes right now (payload plus per-message overhead).
    [[nodiscard]] std::size_t queued_bytes() const noexcept {
        return queued_;
    }

    /// Bytes waiting for the peer to take them, wherever they wait: the queue (charged against
    /// max_queued_bytes) plus what is already framed in the wire buffer and not yet written. A
    /// client that reads nothing keeps its first message in the wire buffer, so queued_bytes()
    /// alone under-counts (ADR-0040).
    [[nodiscard]] std::size_t held_bytes() const noexcept;

    /// Queue a Ping. One may wait at a time: Error{Backpressure} if one is unsent,
    /// Error{InvalidArgument} if payload > 125 bytes, Error{Closing} after queue_close().
    [[nodiscard]] Result<void> queue_ping(ByteView payload);

    /// Queue a Pong behind the unsent ones; with kMaxQueuedPongs already waiting, replace the last
    /// one instead. Precondition: payload <= 125 bytes (the parser guarantees it for a Ping's
    /// payload). Ignored after queue_close(). An out-of-memory error coalesces like a full queue.
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
        bool open = false;        // a streamed message that may still grow
        bool compressed = false;  // permessage-deflate output: RSV1 on the first frame
    };

    void refill();
    [[nodiscard]] bool append_frame(Opcode opcode, bool fin, ByteView payload, bool rsv1 = false);
    /// A data frame can be framed now: the front message has unframed bytes, or has ended.
    [[nodiscard]] bool data_ready() const noexcept;
    void append_control(Opcode opcode, Control& slot);
    void append_pong();
    void append_next_fragment();
    static void fill(Control& slot, ByteView payload) noexcept;

    OutboundConfig config_;
    ByteBuffer wire_;  // framed bytes ready to write
    ByteBuffer data_;  // payloads of queued messages, back to back
    std::deque<Queued> messages_;
    std::size_t front_offset_ = 0;  // bytes of messages_.front() already framed
    bool front_started_ = false;    // its first frame (the one with the opcode) is out
    bool stream_open_ = false;      // the last message is open: nothing else may be queued
    std::size_t queued_ = 0;        // charged against max_queued_bytes
    std::vector<Control> pongs_;    // oldest first; grows on demand, at most kMaxQueuedPongs
    Control ping_;
    Control close_;
    bool close_queued_ = false;
    bool close_in_wire_ = false;
    bool failed_ = false;
};

}  // namespace wsrv
