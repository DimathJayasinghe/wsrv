// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/byte_buffer.hpp"
#include "base/bytes.hpp"
#include "base/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ipc/message.hpp"

namespace wsrv::ipc {

/// Incremental, strict decoder for the IPC stream (ADR-0017). Sans-I/O: feed it whatever the socket
/// returned with next() until it reports that it needs more bytes.
///
///  - The 6-byte header is judged the moment it is complete, BEFORE any body byte is read: length
///    below 2, above the limit, nonzero flags or an unknown type end the stream. A header declaring
///    64 MiB costs nothing: nothing is reserved from a declared length.
///  - A body that arrives in one piece is returned as a view into the caller's input (no copy).
///    A body split across reads is gathered in an internal buffer bounded by the limit.
///  - Chunking never changes the result: byte-at-a-time equals all-at-once.
///  - After a failure every call returns the same failure. The connection must be dropped; there is
///    no way to resynchronise a length-prefixed stream.
///
/// The returned message's views are valid until the next call to next().
/// Thread-affinity: loop-thread-only (not synchronised).
class FrameDecoder {
public:
    /// @param max_frame largest accepted `length` field (clamped to kMaxFrameCeiling).
    explicit FrameDecoder(std::size_t max_frame) noexcept;

    /// Consume bytes from the front of `input` (advanced past what was consumed) and return the
    /// next message, nothing if more bytes are needed, or the failure: Error{InvalidFrame} for a
    /// malformed stream, Error{BufferFull} for a frame above the limit, Error{ResourceExhausted} if
    /// the gather buffer cannot grow.
    [[nodiscard]] Result<std::optional<Message>> next(ByteView& input);

    [[nodiscard]] bool failed() const noexcept {
        return failure_.has_value();
    }

private:
    [[nodiscard]] Result<void> judge_header();
    [[nodiscard]] Result<std::optional<Message>> finish(ByteView body);
    [[nodiscard]] std::unexpected<Error> stop(Error error) noexcept;

    std::size_t max_frame_;
    std::array<std::byte, kFrameHeaderSize> header_{};
    std::size_t have_ = 0;  // header bytes collected
    bool in_body_ = false;
    FrameType type_ = FrameType::Ping;
    std::size_t body_size_ = 0;        // declared body bytes
    ByteBuffer gathered_;              // body of a frame that arrived in pieces
    bool gathered_delivered_ = false;  // a message points into gathered_; clear it on the next call
    std::optional<Error> failure_;
};

}  // namespace wsrv::ipc
