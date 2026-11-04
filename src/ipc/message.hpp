// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/byte_buffer.hpp"
#include "base/bytes.hpp"
#include "base/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace wsrv::ipc {

/// IPC protocol v0 (ADR-0017 section 2; docs/IPC_PROTOCOL.md). Every frame on the wire is
///
///     u32 length   big-endian; counts the bytes AFTER this field: type + flags + body (>= 2)
///     u8  type     FrameType
///     u8  flags    reserved, must be 0
///     ... body     layout per type, all integers big-endian, no padding, no trailing bytes
///
/// The format is canonical: one message has exactly one encoding, so encode(parse(frame)) equals
/// the frame. That is what lets the fuzz target check the parser against the encoder.
inline constexpr std::uint16_t kVersionMajor = 0;
inline constexpr std::uint16_t kVersionMinor = 2;  // 0.2 added the handshake hook (ADR-0018)
/// The first minor that can carry handshake questions and answers.
inline constexpr std::uint16_t kHandshakeHookMinor = 2;
inline constexpr std::size_t kFrameHeaderSize = 6;
/// No setting may raise the frame limit above this (ADR-0017: hard cap 64 MiB).
inline constexpr std::size_t kMaxFrameCeiling = std::size_t{64} << 20U;
inline constexpr std::size_t kMaxTokenSize = 256;
/// RFC 6455 section 5.5: a control frame payload is at most 125 bytes, 2 of them the close code.
inline constexpr std::size_t kMaxCloseReason = 123;
/// The longest subprotocol name an answer may carry: the core only accepts a name the client
/// offered, and a name that long is no real protocol (RFC 6455 section 4.1 says a token).
inline constexpr std::size_t kMaxSubprotocol = 256;

enum class FrameType : std::uint8_t {
    Hello = 1,       ///< client -> daemon: version, token
    HelloOk = 2,     ///< daemon -> client: version, frame limit
    SendText = 3,    ///< client -> daemon
    SendBinary = 4,  ///< client -> daemon
    Close = 5,       ///< client -> daemon: start the closing handshake
    Ping = 6,
    Pong = 7,
    Event = 8,             ///< daemon -> client: one wsrv_event
    Status = 9,            ///< daemon -> client: the answer to one command
    HandshakeAccept = 10,  ///< client -> daemon: let an upgrade in (ADR-0018)
    HandshakeReject = 11,  ///< client -> daemon: refuse an upgrade with an HTTP status
};

/// Same kinds, same numbers, as wsrv_event_type in include/wsrv/wsrv.h.
enum class EventKind : std::uint8_t {
    Open = 1,
    Text = 2,
    Binary = 3,
    Close = 4,
    Error = 5,
    Handshake = 6  ///< data = the blob of capi/handshake_blob.hpp; conn = the id if accepted
};

// Messages. `ByteView` members point into the buffer the message was parsed from.

struct Hello {  ///< u16 major, u16 minor, u16 token length, token
    std::uint16_t major = kVersionMajor;
    std::uint16_t minor = kVersionMinor;
    ByteView token;  ///< at most kMaxTokenSize bytes
};
struct HelloOk {  ///< u16 major, u16 minor, u32 max frame length
    std::uint16_t major = kVersionMajor;
    std::uint16_t minor = kVersionMinor;
    std::uint32_t max_frame = 0;
};
struct SendData {  ///< u32 request id, u64 connection id, payload to the end
    std::uint32_t request = 0;
    std::uint64_t conn = 0;  ///< never 0
    bool text = true;        ///< SendText when true, SendBinary when false
    ByteView data;
};
struct CloseCommand {  ///< u32 request id, u64 connection id, u16 code, reason to the end
    std::uint32_t request = 0;
    std::uint64_t conn = 0;  ///< never 0
    std::uint16_t code = 1000;
    ByteView reason;  ///< at most kMaxCloseReason bytes
};
struct HandshakeAccept {  ///< u32 request id, u64 connection id, subprotocol to the end (may be
                          ///< empty)
    std::uint32_t request = 0;
    std::uint64_t conn = 0;  ///< never 0
    ByteView subprotocol;    ///< at most kMaxSubprotocol bytes; empty = none
};
struct HandshakeReject {  ///< u32 request id, u64 connection id, u16 HTTP status
    std::uint32_t request = 0;
    std::uint64_t conn = 0;    ///< never 0
    std::uint16_t status = 0;  ///< the core decides which statuses are allowed
};
struct Ping {};  ///< empty body
struct Pong {};  ///< empty body
struct Event {   ///< u8 kind, u64 conn, u16 close code, u8 clean, data to the end
    EventKind kind = EventKind::Open;
    std::uint64_t conn = 0;        ///< never 0
    std::uint16_t close_code = 0;  ///< 0 unless kind == Close
    bool clean = false;            ///< false unless kind == Close
    ByteView data;
};
struct Status {  ///< u32 request id, u8 status (a wsrv_status value, mapped by the daemon)
    std::uint32_t request = 0;
    std::uint8_t status = 0;
};

using Message = std::variant<Hello,
                             HelloOk,
                             SendData,
                             CloseCommand,
                             Ping,
                             Pong,
                             Event,
                             Status,
                             HandshakeAccept,
                             HandshakeReject>;

/// @return the FrameType for a wire byte, or nothing for an unknown value.
[[nodiscard]] std::optional<FrameType> frame_type_from(std::uint8_t raw) noexcept;

/// Append one whole frame for `message` to `out`, or leave `out` untouched.
/// @return Error{BufferFull} if the frame would exceed `max_frame` (the u32 length field) or does
/// not fit
///         in `out`; Error{InvalidArgument} for a message no peer could parse (token or close
///         reason too long, connection id 0, impossible event fields); Error{ResourceExhausted} on
///         allocation failure.
/// Thread-affinity: any-thread (pure).
[[nodiscard]] Result<void> encode(const Message& message, std::size_t max_frame, ByteBuffer& out);

/// Parse the body of a frame whose header was already validated. Strict: wrong size, trailing
/// bytes, unknown event kinds, a connection id of 0 or inconsistent Event fields are
/// Error{InvalidFrame}. The returned message's views point into `body`. UTF-8 and close codes are
/// NOT judged here: the core decides those (ADR-0016 section 9) and answers with a Status.
[[nodiscard]] Result<Message> parse_message(FrameType type, ByteView body);

}  // namespace wsrv::ipc
