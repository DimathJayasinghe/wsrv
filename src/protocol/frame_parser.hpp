// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "protocol/failure.hpp"
#include "protocol/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wsrv {

struct FrameParserConfig {
    /// A server parsing client frames requires every frame to be masked (and the default is the
    /// server's); a client parsing server frames sets this to false and then requires every frame
    /// to be unmasked.
    bool expect_masked = true;
    /// Allow RSV1 on the first frame of a data message (permessage-deflate was negotiated). RSV1 on
    /// a continuation or control frame, and RSV2/RSV3 at all, are always protocol errors.
    bool allow_rsv1 = false;
    /// Largest declared payload of a single frame; larger -> 1009 as soon as the header is
    /// complete.
    std::uint64_t max_frame_payload = 1ULL << 20;
};

/// One step of parsing. `header` is valid for every event (it describes the current frame).
/// `payload` is valid only for Payload events: an already UNMASKED view into the caller's buffer.
struct FrameEvent {
    enum class Kind : std::uint8_t { Header, Payload, End };
    Kind kind = Kind::Header;
    FrameHeader header;
    ByteView payload;
};

/// Incremental, strict WebSocket frame parser (RFC 6455 section 5). Sans-I/O: feed it a buffer with
/// next() until it reports that it needs more bytes.
///
///  - Every frame yields Header, zero or more Payload slices, then End (a zero-length frame is
///  Header, End).
///  - The header is validated the moment it is complete, before any payload: reserved opcode / RSV
///  bits /
///    mask expectation / control-frame rules / minimal length encoding / the length limit.
///    Violations that need only the first two bytes are reported after two bytes. The parser NEVER
///    buffers a payload and never allocates: a header declaring 2^40 bytes costs nothing.
///  - Payload is unmasked IN PLACE in the caller's buffer and returned as views into it. Those
///  views are
///    valid until the caller modifies or releases the buffer; the caller must finish with one
///    event's payload before calling next() again if it keeps no copy.
///  - Chunking never changes the result: byte-at-a-time equals all-at-once.
///
/// Thread-affinity: loop-thread-only (not synchronised).
class FrameParser {
public:
    explicit FrameParser(FrameParserConfig config = {}) noexcept : config_{config} {}

    /// Consume bytes from the front of `input` (the span is advanced past what was consumed) and
    /// return the next event, std::nullopt if more bytes are needed, or the ProtocolFailure to
    /// answer with. After a failure every call returns the same failure until reset().
    [[nodiscard]] FrameResult<std::optional<FrameEvent>> next(MutableByteView& input);

    [[nodiscard]] bool failed() const noexcept {
        return state_ == State::Failed;
    }

    /// Forget everything (including a failure); ready for a new connection.
    void reset() noexcept;

private:
    enum class State : std::uint8_t { Header, Payload, End, Failed };

    [[nodiscard]] FrameResult<std::optional<FrameEvent>> read_header(MutableByteView& input);
    [[nodiscard]] FrameResult<void> parse_first_two_bytes();
    [[nodiscard]] FrameResult<void> validate_flags();
    [[nodiscard]] FrameResult<void> parse_rest_of_header();
    [[nodiscard]] std::unexpected<ProtocolFailure> fail_with(CloseCode code,
                                                             std::string_view reason);

    FrameParserConfig config_;
    State state_ = State::Header;
    std::array<std::byte, kMaxHeaderSize> buffer_{};
    std::size_t have_ = 0;      // header bytes collected so far
    std::size_t need_ = 2;      // header bytes required in total (known after the first two)
    unsigned length_code_ = 0;  // the 7-bit length field
    FrameHeader header_;
    std::uint64_t remaining_ = 0;  // payload bytes still to come
    std::optional<Masker> masker_;
    ProtocolFailure failure_{CloseCode::ProtocolError, ""};
};

}  // namespace wsrv
