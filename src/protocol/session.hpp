// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "base/clock.hpp"
#include "base/error.hpp"
#include "protocol/close_code.hpp"
#include "protocol/close_payload.hpp"
#include "protocol/failure.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/message_assembler.hpp"
#include "protocol/outbound_scheduler.hpp"
#include "protocol/utf8.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wsrv {

enum class SessionState : std::uint8_t {
    Open,     ///< messages flow both ways
    Closing,  ///< a Close was sent and/or received; waiting for the handshake to finish
    Closed,   ///< terminal: the transport can be shut down; nothing more is read or written
};

struct SessionConfig {
    FrameParserConfig parser;
    AssemblerConfig assembler;
    OutboundConfig outbound;

    /// 0 disables the timer. Defaults from docs/REQUIREMENTS.md section 4.
    std::chrono::milliseconds ping_interval{30000};  ///< quiet this long -> send a Ping
    std::chrono::milliseconds pong_timeout{10000};   ///< no Pong this long after a Ping -> 1001
    std::chrono::milliseconds idle_timeout{60000};   ///< no complete frame this long -> 1001
    std::chrono::milliseconds close_timeout{5000};   ///< Closing this long -> force Closed
    std::chrono::milliseconds incomplete_message_timeout{30000};  ///< unfinished message -> 1008

    /// Per one-second window (see tick()); more -> 1008. 0 disables.
    std::uint32_t max_control_frames_per_second = 100;
    std::uint32_t max_data_frames_per_second = 10000;
};

/// Hard ceilings (SECURITY_DESIGN section 3: no "unlimited", misconfiguration is capped).
inline constexpr std::uint64_t kMaxConfiguredFrame = 64ULL << 20;
inline constexpr std::size_t kMaxConfiguredMessage = std::size_t{1} << 30;
inline constexpr std::size_t kMaxConfiguredQueue = std::size_t{1} << 30;
inline constexpr std::chrono::milliseconds kMaxConfiguredDuration = std::chrono::hours{24};

/// Check a SessionConfig against the ceilings above. Rules: every duration within [0, 24 h]
/// (larger values overflow clock arithmetic and mean "never" anyway); close_timeout and
/// incomplete_message_timeout greater than zero because FR-C2 and FR-F11 make them mandatory
/// (only ping_interval, pong_timeout and idle_timeout may be 0 = off); frame, message and queue
/// sizes within (0, ceiling]; at least one fragment per message. The server builder calls this
/// and reports the error; ProtocolSession asserts it as a backstop.
/// @return Error{InvalidConfig} whose context names the offending field.
[[nodiscard]] Result<void> validate(const SessionConfig& config);

/// What the session reports to the layer above. Views stay valid until the next call to
/// next_event() / tick() / send_*() / close() / pending_output() on the same session.
struct SessionEvent {
    enum class Kind : std::uint8_t {
        Message,        ///< buffered mode: a complete message (type, data)
        Part,           ///< streaming mode: type, data (+ tail), final
        Ping,           ///< the peer's Ping (a Pong is queued, unless its Close follows in the same
                        ///< read, RFC 6455 section 5.5.3); data = its payload
        Pong,           ///< the peer's Pong; data = its payload
        ProtocolError,  ///< the peer broke the protocol: code, reason. A Close with that code was
                        ///< queued, unless ours was already on its way.
        Closed,         ///< the session is over, exactly once: code, reason, clean
    };
    Kind kind = Kind::Message;
    MessageType type = MessageType::Text;  // Message, Part
    ByteView data;                         // Message, Part, Ping, Pong
    ByteView tail;       // Part: bytes that follow `data` (see MessageEvent::tail)
    bool final = false;  // Part
    /// ProtocolError, Closed. 0 (not a real close code) for every other kind.
    CloseCode code = static_cast<CloseCode>(0);
    std::string_view reason;  // ProtocolError, Closed (never peer-controlled for errors)
    bool clean = false;       // Closed: the closing handshake completed
};

/// The WebSocket connection state machine above frames and messages (RFC 6455 sections 5.5, 7),
/// sans-I/O and deterministic: the same bytes, calls and ticks always give the same events and
/// output. The caller does the I/O:
///
///   read bytes   -> while (auto e = session.next_event(input)) handle(*e);
///   timer / loop -> session.tick(now);
///   app sends    -> session.send_text(..), close(..), ...
///   writable     -> write(session.pending_output()); session.consume_output(n);
///   after consume_output() / tick() / on_transport_closed(): call next_event() with an empty input
///   to collect the events they produced (Closed, ProtocolError).
///
/// Behaviour (see docs/PROTOCOL_EDGE_CASES.md section 4): a Ping is answered with a Pong at the
/// next frame boundary; a peer Close is validated and echoed, then Closed is reported once our
/// Close is written; a protocol violation queues one Close with the matching code, reports
/// ProtocolError and then ignores the peer; after our own Close() the peer's data is dropped until
/// its Close arrives; after the peer's Close nothing more is read. Exactly one Close frame is ever
/// sent and nothing follows it.
///
/// The server role only: inbound frames must be masked, outbound frames are not.
/// Thread-affinity: loop-thread-only (not synchronised).
class ProtocolSession {
public:
    /// The session starts Open; the HTTP handshake is already done. `now` is the start of time for
    /// every timer.
    /// Precondition (asserted): validate(config) succeeds.
    ProtocolSession(SessionConfig config, TimePoint now);

    /// Consume bytes from the front of `input` (it is advanced past what was used and modified in
    /// place: frames are unmasked there) until one event is ready or the input is exhausted.
    /// Call again until it returns std::nullopt. Also returns events queued by other calls.
    [[nodiscard]] std::optional<SessionEvent> next_event(MutableByteView& input);

    /// Advance the session's clock. Never moves backwards: earlier values are ignored. Call at
    /// least once per second while the connection is open and before each next_event() batch:
    /// the rate-limit windows are measured in ticks.
    void tick(TimePoint now);

    /// @return Error{Closing} unless Open; Error{InvalidUtf8} for non-UTF-8 text;
    /// Error{Backpressure} / Error{BufferFull} when the outbound queue cannot take the message.
    /// Nothing is queued on error.
    [[nodiscard]] Result<void> send_text(std::string_view text);
    [[nodiscard]] Result<void> send_binary(ByteView data);

    /// Streaming send (ADR-0015): a message whose bytes are produced piece by piece, sent as
    /// fragments (RFC 6455 section 5.4) without holding the whole message in memory.
    /// begin_message(Text|Binary) opens it; write_part() adds bytes; end_message() finishes it.
    /// While it is open send_text/send_binary/begin_message fail with Error{MessageInProgress};
    /// control frames still go out between fragments. Closing the session discards it.
    /// @return Error{Closing} unless Open; Error{MessageInProgress}; Error{Backpressure} /
    /// Error{ResourceExhausted} as for send_text.
    [[nodiscard]] Result<void> begin_message(Opcode text_or_binary);
    /// Text parts are checked as UTF-8 across part boundaries (a character may be split between
    /// parts). A refused part is queued not at all and the message stays open.
    /// @return Error{InvalidUtf8}, Error{Backpressure} (retry after the queue drained),
    /// Error{BufferFull} (the part can never fit), Error{InvalidArgument} (no open message),
    /// Error{Closing}.
    [[nodiscard]] Result<void> write_part(ByteView bytes);
    /// @return Error{InvalidUtf8} if a text message would end inside a character (it stays open),
    /// Error{InvalidArgument} (no open message), Error{Closing}.
    [[nodiscard]] Result<void> end_message();
    [[nodiscard]] bool message_open() const noexcept {
        return out_.stream_open();
    }
    /// Data bytes waiting in the outbound queue (not yet framed), including per-message overhead.
    [[nodiscard]] std::size_t queued_output_bytes() const noexcept {
        return out_.queued_bytes();
    }
    /// Payload at most 125 bytes (Error{InvalidArgument}); one unsent Ping at a time
    /// (Error{Backpressure}).
    [[nodiscard]] Result<void> send_ping(ByteView payload);

    /// Start the closing handshake. Idempotent: a no-op once closing or closed.
    /// @return Error{InvalidArgument} if `code` must not be sent (1005, 1006, 1015, unassigned...)
    /// or `reason` is not UTF-8; the reason is cut to 123 bytes.
    [[nodiscard]] Result<void> close(CloseCode code, std::string_view reason);

    /// The transport ended (EOF, reset, write failure). Closed is reported at once.
    void on_transport_closed();

    /// Bytes to write now; empty if none. Call consume_output() with what was accepted.
    [[nodiscard]] ByteView pending_output();
    void consume_output(std::size_t count);

    [[nodiscard]] SessionState state() const noexcept {
        return state_;
    }

private:
    /// Tiny FIFO for events that are produced outside next_event() (bounded: 4 is the most any
    /// sequence can produce, because Closed is terminal).
    class EventQueue {
    public:
        void push(const SessionEvent& event);
        [[nodiscard]] std::optional<SessionEvent> pop();

    private:
        std::array<SessionEvent, 4> items_{};
        std::size_t head_ = 0;
        std::size_t size_ = 0;
    };

    [[nodiscard]] std::optional<SessionEvent> pump(MutableByteView& input);
    [[nodiscard]] std::optional<SessionEvent> on_frame(const FrameEvent& frame);
    [[nodiscard]] std::optional<SessionEvent> translate(const MessageEvent& event);
    [[nodiscard]] std::optional<SessionEvent> on_control(const MessageEvent& event);
    [[nodiscard]] std::optional<SessionEvent> on_peer_close(ByteView payload);
    [[nodiscard]] std::optional<SessionEvent> fail_connection(const ProtocolFailure& failure);
    void begin_close(std::optional<std::uint16_t> code, std::string_view reason);
    void check_close_completion();
    void emit_closed(bool clean);
    void give_up();
    [[nodiscard]] std::optional<ProtocolFailure> count_frame(const FrameEvent& frame);
    void track_message(const FrameEvent& frame);
    void run_timers();
    void heartbeat();
    [[nodiscard]] static bool
    elapsed(TimePoint now, TimePoint since, std::chrono::milliseconds limit);

    SessionConfig config_;
    FrameParser parser_;
    MessageAssembler assembler_;
    OutboundScheduler out_;
    Utf8Validator stream_utf8_;  // the text message being streamed out; unused for binary
    bool stream_is_text_ = false;
    EventQueue queue_;
    SessionState state_ = SessionState::Open;
    TimePoint now_;

    bool close_sent_ = false;      // our Close is queued (or written)
    bool close_received_ = false;  // the peer's Close arrived (valid)
    bool failed_ = false;          // the peer violated the protocol; input is ignored
    std::uint16_t sent_code_ = 0;  // the code of our Close
    std::string_view failure_reason_;
    bool failure_close_sent_ = false;  // the Close we sent was caused by the failure
    // timers and rates (all measured against now_, which only tick() advances)
    TimePoint last_frame_;                      // last complete frame received (idle)
    TimePoint last_ping_;                       // last Ping we sent (heartbeat)
    TimePoint closing_since_;                   // when state_ left Open (close timeout)
    TimePoint window_start_;                    // start of the current rate-limit window
    std::optional<TimePoint> ping_sent_at_;     // a Ping is waiting for its Pong
    std::optional<TimePoint> message_started_;  // a fragmented message is in progress since
    std::uint32_t control_in_window_ = 0;
    std::uint32_t data_in_window_ = 0;

    std::optional<std::uint16_t> peer_code_;
    std::array<char, kMaxCloseReason> peer_reason_{};
    std::size_t peer_reason_length_ = 0;
};

}  // namespace wsrv
