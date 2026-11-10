// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/byte_buffer.hpp"
#include "base/bytes.hpp"
#include "protocol/failure.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/opcode.hpp"
#include "protocol/utf8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wsrv {

enum class MessageType : std::uint8_t { Text, Binary };

struct AssemblerConfig {
    /// Largest message after reassembly; larger -> 1009, decided when the frame HEADER that would
    /// exceed it arrives (before its payload).
    std::size_t max_message_size = 1U << 20;
    /// Most data frames (fragments) in one message; more -> 1008.
    std::size_t max_fragments = 1024;
    /// false (default): deliver whole messages (bounded by max_message_size). true: deliver Parts
    /// as they arrive with O(chunk) memory; text parts always end on a code-point boundary.
    bool streaming = false;
};

/// What the assembler hands to the application layer. `data` is valid only until the next handle()
/// call.
struct MessageEvent {
    enum class Kind : std::uint8_t {
        Message,  ///< buffered mode: a complete message (data = the whole payload)
        Part,     ///< streaming mode: some bytes of a message; `final` marks the last part
        Control,  ///< a complete Close / Ping / Pong frame (data = its payload, control_opcode says
                  ///< which)
    };
    Kind kind = Kind::Message;
    MessageType type = MessageType::Text;  // Message and Part
    ByteView data;
    /// Part: bytes that follow `data` in the same part. Only set when a character held back from
    /// the previous chunk was completed: `data` is then that 1-4 byte character (from an internal
    /// array) and `tail` the rest of the chunk, still a view into the caller's buffer. Both are
    /// code-point aligned, so a consumer may deliver them as two parts or join them.
    ByteView tail;
    bool final = false;                    // Part: last part of the message
    Opcode control_opcode = Opcode::Ping;  // Control
};

/// Turns FrameParser events into messages (RFC 6455 section 5.4) with the rules of
/// docs/FRAGMENTATION.md:
///  - a continuation without a message in progress, or a new data frame while one is in progress ->
///  1002;
///  - control frames may arrive between fragments and never disturb the message in progress;
///  - size (1009) and fragment-count (1008) limits are checked when a frame header arrives, before
///  its payload;
///  - text is validated as UTF-8 incrementally and fails (1007) at the first bad byte, even
///  mid-fragment;
///    a character may span fragments (and interleaved control frames).
/// A Close frame does not end the message state by itself: the session calls discard_message().
/// After that the rest of the message (the remainder of the current frame, then its continuation
/// frames up to FIN) is read and counted against the limits but never delivered or validated, so
/// the peer may keep sending it until its own Close arrives (RFC 6455 section 5.5.1, EC-F30).
/// After a failure every call fails until reset().
///
/// Thread-affinity: loop-thread-only (not synchronised).
class MessageAssembler {
public:
    explicit MessageAssembler(AssemblerConfig config = {});

    /// Process one parser event. Returns the event for the application, std::nullopt if there is
    /// nothing to report yet, or the failure to answer with.
    [[nodiscard]] FrameResult<std::optional<MessageEvent>> handle(const FrameEvent& event);

    /// True while a fragmented message has started and not yet finished.
    [[nodiscard]] bool in_message() const noexcept {
        return in_message_;
    }

    /// Drop the message in progress, wherever it is, even in the middle of a frame (used when the
    /// session decides to close). Nothing more of that message is delivered; the frames that
    /// complete it are drained silently. A no-op when no message is in progress.
    /// Safe to call while a Message view is in use: the storage is released by the next handle(),
    /// not by this call (a handler may close() from inside its callback).
    void discard_message() noexcept;

    /// Forget everything, including a failure.
    void reset() noexcept;

private:
    using StepResult = FrameResult<std::optional<MessageEvent>>;

    void reset_message_state() noexcept;

    [[nodiscard]] std::unexpected<ProtocolFailure> fail_with(CloseCode code,
                                                             std::string_view reason);
    [[nodiscard]] StepResult on_header(const FrameHeader& header);
    [[nodiscard]] StepResult on_payload(const FrameEvent& event);
    [[nodiscard]] StepResult on_end(const FrameHeader& header);
    [[nodiscard]] FrameResult<void> start_or_continue(const FrameHeader& header);
    [[nodiscard]] StepResult buffer_payload(ByteView chunk);
    [[nodiscard]] StepResult stream_payload(ByteView chunk, bool last_of_message);
    [[nodiscard]] StepResult stream_text_payload(ByteView chunk, bool last_of_message);
    [[nodiscard]] std::size_t complete_held_character(ByteView chunk);

    AssemblerConfig config_;
    bool failed_ = false;
    ProtocolFailure failure_{CloseCode::ProtocolError, ""};

    // message state
    bool in_message_ = false;
    bool draining_ = false;  // discarded: swallow the rest of the message up to its FIN frame
    MessageType type_ = MessageType::Text;
    std::size_t fragments_ = 0;
    std::size_t total_ =
        0;  // payload bytes received for the message; invariant: <= max_message_size
    Utf8Validator utf8_;
    ByteBuffer buffer_;          // buffered mode: the message so far
    bool clear_buffer_ = false;  // the delivered message's view stays valid until the next handle()
    bool final_part_sent_ = false;
    std::array<std::byte, 3> held_{};  // streaming text: bytes of an unfinished character
    std::size_t held_len_ = 0;
    std::array<std::byte, 4>
        join_{};  // streaming text: the held bytes + the bytes that finish them

    // frame state
    FrameHeader frame_;
    std::uint64_t frame_received_ = 0;
    std::array<std::byte, kMaxControlPayload> control_{};
    std::size_t control_len_ = 0;
};

}  // namespace wsrv
