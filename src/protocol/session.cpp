// SPDX-License-Identifier: Apache-2.0
#include "protocol/session.hpp"

#include "base/checked.hpp"
#include "protocol/utf8.hpp"

#include <algorithm>
#include <limits>

namespace wsrv {

namespace {

[[nodiscard]] SessionEvent make_error(CloseCode code, std::string_view reason) {
    SessionEvent event;
    event.kind = SessionEvent::Kind::ProtocolError;
    event.code = code;
    event.reason = reason;
    return event;
}

[[nodiscard]] SessionEvent make_data_event(const MessageEvent& e) {
    SessionEvent event;
    event.kind = e.kind == MessageEvent::Kind::Message ? SessionEvent::Kind::Message
                                                       : SessionEvent::Kind::Part;
    event.type = e.type;
    event.data = e.data;
    event.tail = e.tail;
    event.final = e.final;
    return event;
}

}  // namespace

void ProtocolSession::EventQueue::push(const SessionEvent& event) {
    WSRV_ASSERT(size_ < items_.size());
    items_[(head_ + size_) % items_.size()] = event;
    ++size_;
}

std::optional<SessionEvent> ProtocolSession::EventQueue::pop() {
    if (size_ == 0) {
        return std::nullopt;
    }
    const SessionEvent event = items_[head_];
    head_ = (head_ + 1) % items_.size();
    --size_;
    return event;
}

namespace {

[[nodiscard]] Error bad_config(std::string_view field) {
    return Error{ErrorCode::InvalidConfig, field};
}

[[nodiscard]] bool duration_ok(std::chrono::milliseconds value) {
    return value >= std::chrono::milliseconds::zero() && value <= kMaxConfiguredDuration;
}

[[nodiscard]] bool in_range(std::uint64_t value, std::uint64_t ceiling) {
    return value > 0 && value <= ceiling;
}

[[nodiscard]] Result<void> validate_timeouts(const SessionConfig& config) {
    for (const std::chrono::milliseconds value : {config.ping_interval,
                                                  config.pong_timeout,
                                                  config.idle_timeout,
                                                  config.close_timeout,
                                                  config.incomplete_message_timeout}) {
        if (!duration_ok(value)) {
            return std::unexpected{bad_config("a timeout is negative or longer than 24 hours")};
        }
    }
    if (config.close_timeout == std::chrono::milliseconds::zero()) {
        return std::unexpected{bad_config("close_timeout is mandatory (FR-C2)")};
    }
    if (config.incomplete_message_timeout == std::chrono::milliseconds::zero()) {
        return std::unexpected{bad_config("incomplete_message_timeout is mandatory (FR-F11)")};
    }
    return {};
}

[[nodiscard]] Result<void> validate_sizes(const SessionConfig& config) {
    if (!in_range(config.parser.max_frame_payload, kMaxConfiguredFrame) ||
        !in_range(config.outbound.max_frame_payload, kMaxConfiguredFrame)) {
        return std::unexpected{bad_config("a frame payload limit is zero or above 64 MiB")};
    }
    if (!in_range(config.assembler.max_message_size, kMaxConfiguredMessage) ||
        config.assembler.max_fragments == 0) {
        return std::unexpected{bad_config("max_message_size or max_fragments out of range")};
    }
    if (config.outbound.max_queued_bytes < kMessageOverhead ||
        config.outbound.max_queued_bytes > kMaxConfiguredQueue) {
        return std::unexpected{bad_config("max_queued_bytes out of range")};
    }
    return {};
}

[[nodiscard]] Result<void> validate_deflate(const SessionDeflate& deflate) {
    const auto window_ok = [](int bits) {
        return bits >= kMinDeflateWindowBits && bits <= kMaxDeflateWindowBits;
    };
    if (deflate.codecs == nullptr || !window_ok(deflate.params.server_window_bits) ||
        !window_ok(deflate.params.client_window_bits)) {
        return std::unexpected{bad_config("deflate needs a codec factory and windows 9..15")};
    }
    if (deflate.max_inflate_ratio == 0 || deflate.max_inflate_ratio > kMaxConfiguredInflateRatio) {
        return std::unexpected{bad_config("max_inflate_ratio out of range")};
    }
    return {};
}

}  // namespace

Result<void> validate(const SessionConfig& config) {
    if (const Result<void> timeouts = validate_timeouts(config); !timeouts) {
        return timeouts;
    }
    if (const Result<void> sizes = validate_sizes(config); !sizes) {
        return sizes;
    }
    if (config.deflate) {
        return validate_deflate(*config.deflate);
    }
    return {};
}

namespace {

[[nodiscard]] SessionConfig checked(const SessionConfig& config) {
    WSRV_ASSERT(validate(config).has_value());  // the server builder reports this error earlier
    return config;
}

}  // namespace

namespace {

/// RSV1 is legal on the first frame of a data message exactly when permessage-deflate was agreed.
[[nodiscard]] FrameParserConfig parser_config(const SessionConfig& config) {
    FrameParserConfig parser = config.parser;
    parser.allow_rsv1 = parser.allow_rsv1 || config.deflate.has_value();
    return parser;
}

}  // namespace

ProtocolSession::ProtocolSession(const SessionConfig& config, TimePoint now)
    : config_{checked(config)}, parser_{parser_config(config)}, assembler_{config.assembler},
      out_{config.outbound}, now_{now}, last_frame_{now}, last_ping_{now}, closing_since_{now},
      window_start_{now} {}

// ---- input ------------------------------------------------------------------------------------

std::optional<SessionEvent> ProtocolSession::next_event(MutableByteView& input) {
    if (auto queued = queue_.pop()) {
        return queued;
    }
    return pump(input);
}

/// Read frames until one produces an event. Once the peer's Close has arrived, or the peer has
/// violated the protocol, the rest of the input is thrown away (FR-C5, EC-C10).
std::optional<SessionEvent> ProtocolSession::pump(MutableByteView& input) {
    while (reading()) {
        if (inflate_.phase != InflatePhase::Idle) {  // a compressed frame is still being inflated
            if (auto event = or_queued(run_inflate())) {
                return event;
            }
            continue;
        }
        const FrameResult<std::optional<FrameEvent>> frame = parser_.next(input);
        if (!frame) {
            return fail_connection(frame.error());
        }
        const std::optional<FrameEvent>& parsed = *frame;
        if (!parsed) {
            return std::nullopt;  // needs more bytes
        }
        if (auto event = or_queued(on_frame(*parsed))) {
            return event;
        }
    }
    input = input.subspan(input.size());
    return queue_.pop();
}

/// `event` if there is one, else whatever an earlier call left in the queue (Closed, for example).
std::optional<SessionEvent> ProtocolSession::or_queued(std::optional<SessionEvent> event) {
    return event ? event : queue_.pop();
}

std::optional<SessionEvent> ProtocolSession::on_frame(const FrameEvent& frame) {
    if (const std::optional<ProtocolFailure> over_limit = count_frame(frame)) {
        return fail_connection(*over_limit);
    }
    if (config_.deflate && frame_is_compressed(frame)) {
        return on_compressed_frame(frame);
    }
    return feed_assembler(frame, frame);
}

/// `event` goes to the assembler; `real` is the frame the parser produced (the incomplete-message
/// timer follows real frames; for an uncompressed frame the two are the same).
std::optional<SessionEvent> ProtocolSession::feed_assembler(const FrameEvent& event,
                                                            const FrameEvent& real) {
    const auto step = assembler_.handle(event);
    track_message(real);
    if (!step) {
        return fail_connection(step.error());
    }
    const std::optional<MessageEvent>& message = *step;
    if (!message) {
        return std::nullopt;
    }
    return translate(*message);
}

// ---- permessage-deflate, receive side (RFC 7692 section 6.2, ADR-0020 section 4) ----------------

/// RSV1 on the first frame of a data message starts a compressed message; its continuation frames
/// belong to it (RSV1 on a continuation or control frame never gets here: the parser refuses it).
bool ProtocolSession::frame_is_compressed(const FrameEvent& frame) {
    const FrameHeader& header = frame.header;
    if (is_control(header.opcode)) {
        return false;  // control frames are never compressed (section 6.1)
    }
    if (frame.kind == FrameEvent::Kind::Header && header.opcode != Opcode::Continuation) {
        inflate_.message_compressed = header.rsv1;
    }
    return inflate_.message_compressed;
}

const SessionDeflate& ProtocolSession::deflate_config() const {
    WSRV_ASSERT(config_.deflate.has_value());
    return *config_.deflate;
}

DeflateCodecs& ProtocolSession::active_codecs() {
    WSRV_ASSERT(codecs_.has_value());
    return *codecs_;
}

Result<void> ProtocolSession::ensure_codecs() {
    if (codecs_) {
        return {};
    }
    const SessionDeflate& deflate = deflate_config();
    auto created = deflate.codecs->create(deflate.params);
    if (!created) {
        return std::unexpected{created.error()};
    }
    try {
        inflate_buffer_.resize(kInflatePiece);
        codecs_.emplace(std::move(*created));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "deflate: out of memory");
    }
    return {};
}

namespace {

/// What the assembler is shown in place of a compressed frame: no RSV1, and no declared length
/// (the inflated length is not known in advance; the size limit is checked on the bytes).
[[nodiscard]] FrameHeader inflated_header(const FrameHeader& real) {
    FrameHeader header = real;
    header.rsv1 = false;
    header.payload_length = 0;
    return header;
}

}  // namespace

std::optional<SessionEvent> ProtocolSession::on_compressed_frame(const FrameEvent& frame) {
    switch (frame.kind) {
    case FrameEvent::Kind::Header: {
        if (const Result<void> ready = ensure_codecs(); !ready) {
            return inflate_failure(ready.error());
        }
        if (frame.header.opcode != Opcode::Continuation) {
            inflate_.compressed_in = 0;
            inflate_.inflated_out = 0;
        }
        inflate_.header = frame.header;
        return feed_assembler(FrameEvent{frame.kind, inflated_header(frame.header), {}}, frame);
    }
    case FrameEvent::Kind::Payload:
        inflate_.pending = frame.payload;
        inflate_.phase = InflatePhase::Payload;
        return run_inflate();
    case FrameEvent::Kind::End:
        inflate_.phase = frame.header.fin ? InflatePhase::Finish : InflatePhase::End;
        return run_inflate();
    }
    return std::nullopt;
}

std::optional<SessionEvent> ProtocolSession::inflate_failure(const Error& error) {
    inflate_.phase = InflatePhase::Idle;
    if (error.code == ErrorCode::InvalidCompressedData) {
        return fail_connection(
            ProtocolFailure{CloseCode::InvalidPayload, "invalid compressed data"});
    }
    return fail_connection(ProtocolFailure{CloseCode::InternalError, "cannot inflate the message"});
}

/// Inflate in pieces of at most kInflatePiece and give each to the assembler, which applies the
/// message-size limit, UTF-8 validation and streaming to the inflated bytes. Returns as soon as a
/// piece produces an event; the phase and `pending` remember where to resume (pump() calls again).
std::optional<SessionEvent> ProtocolSession::run_inflate() {
    const FrameHeader header = inflated_header(inflate_.header);
    const FrameEvent real{FrameEvent::Kind::End, inflate_.header, {}};
    while (inflate_.phase != InflatePhase::Idle) {
        if (inflate_.phase == InflatePhase::End) {
            return end_inflated_frame();
        }
        const Result<InflateStep> step = inflate_step();
        if (!step) {
            return inflate_failure(step.error());
        }
        if (step->produced == 0) {
            continue;
        }
        inflate_.inflated_out += step->produced;
        if (inflate_ratio_exceeded()) {
            inflate_.phase = InflatePhase::Idle;
            return fail_connection(
                ProtocolFailure{CloseCode::MessageTooBig,
                                "inflated size out of proportion to the compressed size"});
        }
        const FrameEvent piece{
            FrameEvent::Kind::Payload, header, ByteView{inflate_buffer_}.first(step->produced)};
        if (auto event = feed_assembler(piece, real)) {
            return event;
        }
    }
    return std::nullopt;
}

/// One call into the inflater for the current phase, then the bookkeeping of what it used:
/// `compressed_in` feeds the ratio limit, `pending` is where the next call resumes, and the phase
/// moves on when this frame's input is used up with no output waiting (Payload -> Idle) or the
/// inflater says the message is complete (Finish -> End).
Result<InflateStep> ProtocolSession::inflate_step() {
    IMessageInflater& inflater = *active_codecs().inflater;
    const bool finishing = inflate_.phase == InflatePhase::Finish;
    const Result<InflateStep> step = finishing
                                         ? inflater.finish(inflate_buffer_)
                                         : inflater.inflate(inflate_.pending, inflate_buffer_);
    if (!step) {
        return step;
    }
    inflate_.compressed_in += step->consumed;
    inflate_.pending = inflate_.pending.subspan(step->consumed);
    if (finishing) {
        if (step->done) {
            inflate_.phase = InflatePhase::End;
        }
    } else if (inflate_.pending.empty() && step->produced < inflate_buffer_.size()) {
        inflate_.phase = InflatePhase::Idle;
    }
    return step;
}

/// SECURITY_DESIGN T6: the inflated size may not outgrow the compressed size by more than the
/// configured ratio plus a small allowance. Called after every piece, before more output is made.
/// Saturating: a limit that does not fit in 64 bits is above any size that could be produced.
bool ProtocolSession::inflate_ratio_exceeded() const {
    constexpr std::uint64_t kUnlimited = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t scaled =
        checked_mul<std::uint64_t>(inflate_.compressed_in, deflate_config().max_inflate_ratio)
            .value_or(kUnlimited);
    const std::uint64_t allowed =
        checked_add<std::uint64_t>(scaled, kInflateAllowance).value_or(kUnlimited);
    return inflate_.inflated_out > allowed;
}

/// The frame's input is fully inflated: tell the assembler the frame ended. At FIN the message is
/// over, so the next data frame starts a new one (RSV1 is read again).
std::optional<SessionEvent> ProtocolSession::end_inflated_frame() {
    inflate_.phase = InflatePhase::Idle;
    if (inflate_.header.fin) {
        inflate_.message_compressed = false;
    }
    const FrameEvent real{FrameEvent::Kind::End, inflate_.header, {}};
    return feed_assembler(FrameEvent{FrameEvent::Kind::End, inflated_header(inflate_.header), {}},
                          real);
}

std::optional<SessionEvent> ProtocolSession::translate(const MessageEvent& event) {
    if (event.kind == MessageEvent::Kind::Control) {
        return on_control(event);
    }
    if (close_sent_) {
        return std::nullopt;  // we already said goodbye: the peer's data is dropped (EC-C10)
    }
    return make_data_event(event);
}

std::optional<SessionEvent> ProtocolSession::on_control(const MessageEvent& event) {
    if (event.control_opcode == Opcode::Close) {
        return on_peer_close(event.data);
    }
    if (close_sent_) {
        return std::nullopt;  // nothing may follow our Close frame
    }
    SessionEvent out;
    out.data = event.data;
    if (event.control_opcode == Opcode::Ping) {
        out_.queue_pong(event.data);  // same payload, next frame boundary (RFC 6455 section 5.5.3)
        out.kind = SessionEvent::Kind::Ping;
    } else {
        ping_sent_at_.reset();  // the peer is alive
        out.kind = SessionEvent::Kind::Pong;
    }
    return out;
}

/// RFC 6455 section 5.5.1 / 7.1.4: validate, remember the code and reason, echo the code (or an
/// empty Close) unless our own Close is already on its way, and stop reading.
std::optional<SessionEvent> ProtocolSession::on_peer_close(ByteView payload) {
    const FrameResult<std::optional<ClosePayload>> parsed = parse_close_payload(payload);
    if (!parsed) {
        return fail_connection(parsed.error());
    }
    close_received_ = true;
    assembler_.discard_message();  // a half-delivered message is never completed (EC-F26)
    const std::optional<ClosePayload>& close = *parsed;
    if (close) {
        peer_code_ = close->code;
        peer_reason_length_ = close->reason.size();
        std::copy(close->reason.begin(), close->reason.end(), peer_reason_.begin());
    }
    if (!close_sent_) {
        begin_close(peer_code_, {});
    }
    check_close_completion();
    return std::nullopt;
}

/// The peer broke a rule: answer with exactly one Close carrying the code, report it, ignore the
/// peer from now on (RFC 6455 section 7.1.7 "Fail the WebSocket Connection").
std::optional<SessionEvent> ProtocolSession::fail_connection(const ProtocolFailure& failure) {
    failed_ = true;
    failure_reason_ = failure.reason;
    failure_close_sent_ = !close_sent_;
    if (!close_sent_) {
        begin_close(static_cast<std::uint16_t>(failure.code), failure.reason);
    }
    check_close_completion();
    return make_error(failure.code, failure.reason);
}

// ---- closing ----------------------------------------------------------------------------------

void ProtocolSession::begin_close(std::optional<std::uint16_t> code, std::string_view reason) {
    out_.queue_close(code, reason);
    close_sent_ = true;
    sent_code_ = code.value_or(static_cast<std::uint16_t>(CloseCode::NoStatusReceived));
    state_ = SessionState::Closing;
    closing_since_ = now_;
    assembler_.discard_message();
    message_started_.reset();
}

Result<void> ProtocolSession::close(CloseCode code, std::string_view reason) {
    const auto raw = static_cast<std::uint16_t>(code);
    if (!is_valid_on_wire(raw) || !is_valid_utf8(as_bytes(reason))) {
        return fail(ErrorCode::InvalidArgument, "close code or reason must not be sent");
    }
    if (state_ == SessionState::Open) {
        begin_close(raw, reason);
    }
    return {};
}

/// Closed is reported once our Close frame has been written AND the handshake is over: the peer's
/// Close arrived (clean), or we failed the connection (not clean; RFC 6455 section 7.1.7 does not
/// wait for the peer).
void ProtocolSession::check_close_completion() {
    if (state_ != SessionState::Closing || !out_.close_flushed()) {
        return;
    }
    if (close_received_) {
        emit_closed(true);
    } else if (failed_) {
        emit_closed(false);
    }
}

/// Closed.code is the peer's code if it sent one (1005 if none), else the code we sent when we
/// failed the connection, else 1006 (the transport died or the peer never answered).
void ProtocolSession::emit_closed(bool clean) {
    SessionEvent event;
    event.kind = SessionEvent::Kind::Closed;
    event.clean = clean;
    if (close_received_) {
        event.code = static_cast<CloseCode>(peer_code_.value_or(1005));
        event.reason = std::string_view{peer_reason_.data(), peer_reason_length_};
    } else if (failed_ && out_.close_flushed()) {
        event.code = static_cast<CloseCode>(sent_code_);
        if (failure_close_sent_) {
            event.reason = failure_reason_;  // our Close carried it; otherwise it was not sent
        }
    } else {
        event.code = CloseCode::AbnormalClosure;
    }
    state_ = SessionState::Closed;
    queue_.push(event);
}

void ProtocolSession::on_transport_closed() {
    if (state_ != SessionState::Closed) {
        emit_closed(close_received_);  // the peer's Close arrived first: still a clean close
    }
}

// ---- output -----------------------------------------------------------------------------------

Result<void> ProtocolSession::send_text(std::string_view text) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    if (!is_valid_utf8(as_bytes(text))) {
        return fail(ErrorCode::InvalidUtf8, "text message is not valid UTF-8");
    }
    return send_message(Opcode::Text, as_bytes(text));
}

Result<void> ProtocolSession::send_binary(ByteView data) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    return send_message(Opcode::Binary, data);
}

// ---- permessage-deflate, send side (RFC 7692 section 7.2.1, ADR-0020 section 5) ----------------

bool ProtocolSession::should_compress(std::size_t size) const noexcept {
    return config_.deflate && !compress_disabled_ && size >= config_.deflate->min_compress_size &&
           size > 0;
}

namespace {

/// Zlib's worst case for incompressible input with a sync flush (deflateBound for non-default
/// parameters), rounded up. Used to make sure the queue has room BEFORE the compressor sees the
/// message: with context takeover, a message that was compressed but not sent would leave the
/// peer's history one message behind and corrupt every later one.
[[nodiscard]] std::size_t compressed_bound(std::size_t size) noexcept {
    return size + size / 8 + size / 64 + 32;
}

}  // namespace

Result<void> ProtocolSession::send_message(Opcode text_or_binary, ByteView data) {
    if (!should_compress(data.size())) {
        return out_.enqueue_message(text_or_binary, data);
    }
    if (const Result<void> room = out_.can_enqueue(compressed_bound(data.size())); !room) {
        if (room.error().code != ErrorCode::BufferFull) {
            return room;
        }
        // The worst case of compression would not fit, the plain message might: send it plain.
        if (const Result<void> plain = out_.can_enqueue(data.size()); !plain) {
            return plain;
        }
        return out_.enqueue_message(text_or_binary, data);
    }
    if (const Result<void> ready = ensure_codecs(); !ready) {
        compress_disabled_ = true;  // nothing was compressed yet: plain is consistent
        return out_.enqueue_message(text_or_binary, data);
    }
    deflate_scratch_.clear();
    const Result<void> compressed = active_codecs().deflater->deflate(data, deflate_scratch_);
    if (!compressed) {
        compress_disabled_ = true;  // the compressor's history is in doubt: never use it again
        return compressed;
    }
    // With server_no_context_takeover the compressor has no memory, so an expansion can be sent
    // as it was (EC-D07). With takeover the peer's history must see this message: send it as is.
    const bool plain_is_better = deflate_config().params.server_no_context_takeover &&
                                 deflate_scratch_.size() >= data.size();
    const Result<void> queued = plain_is_better
                                    ? out_.enqueue_message(text_or_binary, data)
                                    : out_.enqueue_message(text_or_binary, deflate_scratch_, true);
    if (!queued && !plain_is_better) {
        compress_disabled_ = true;  // compressed but not queued: history diverged from the peer's
    }
    if (deflate_scratch_.capacity() > 4 * kInflatePiece) {
        std::vector<std::byte>{}.swap(deflate_scratch_);  // do not keep a big buffer per connection
    }
    return queued;
}

Result<void> ProtocolSession::begin_message(Opcode text_or_binary) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    if (const Result<void> opened = out_.begin_stream(text_or_binary); !opened) {
        return opened;
    }
    stream_is_text_ = text_or_binary == Opcode::Text;
    stream_utf8_.reset();
    return {};
}

Result<void> ProtocolSession::write_part(ByteView bytes) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    if (!stream_is_text_ || !out_.stream_open()) {
        return out_.append_stream(bytes);
    }
    // Validate on a copy: the part may be refused by the queue after it passed the UTF-8 check,
    // and a refused part must leave no trace (the writer retries it).
    Utf8Validator trial = stream_utf8_;
    if (const Result<void> valid = trial.feed(bytes); !valid) {
        return fail(ErrorCode::InvalidUtf8, "text part is not valid UTF-8");
    }
    if (const Result<void> queued = out_.append_stream(bytes); !queued) {
        return queued;
    }
    stream_utf8_ = trial;
    return {};
}

Result<void> ProtocolSession::end_message() {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    if (stream_is_text_ && out_.stream_open() && !stream_utf8_.at_boundary()) {
        return fail(ErrorCode::InvalidUtf8,
                    "text message ends inside a character");  // RFC 6455 8.1
    }
    return out_.end_stream();
}

Result<void> ProtocolSession::send_ping(ByteView payload) {
    if (state_ != SessionState::Open) {
        return fail(ErrorCode::Closing, "send after close");
    }
    return out_.queue_ping(payload);
}

ByteView ProtocolSession::pending_output() {
    if (state_ == SessionState::Closed) {
        return {};
    }
    const ByteView view = out_.pending();
    if (out_.failed()) {
        give_up();
        return {};
    }
    return view;
}

void ProtocolSession::consume_output(std::size_t count) {
    if (state_ == SessionState::Closed) {
        return;
    }
    out_.consume(count);
    check_close_completion();
}

/// Out of memory while framing output: not even a Close can be written. Report and stop.
void ProtocolSession::give_up() {
    queue_.push(make_error(CloseCode::InternalError, "out of memory while writing"));
    emit_closed(false);
}

// ---- time -------------------------------------------------------------------------------------

bool ProtocolSession::elapsed(TimePoint now, TimePoint since, std::chrono::milliseconds limit) {
    // Compared in milliseconds: widening the limit to nanoseconds could overflow.
    return limit > std::chrono::milliseconds::zero() &&
           std::chrono::duration_cast<std::chrono::milliseconds>(now - since) >= limit;
}

/// Rate limits (SECURITY_DESIGN T7): data frames are counted when their header arrives, control
/// frames when they complete; both per window of at least one second, reset by tick().
std::optional<ProtocolFailure> ProtocolSession::count_frame(const FrameEvent& frame) {
    if (frame.kind == FrameEvent::Kind::End) {
        last_frame_ = now_;
    }
    const bool control = is_control(frame.header.opcode);
    if (!control && frame.kind == FrameEvent::Kind::Header) {
        const std::uint32_t limit = config_.max_data_frames_per_second;
        if (limit != 0 && ++data_in_window_ > limit) {
            return ProtocolFailure{CloseCode::PolicyViolation, "data frame rate limit exceeded"};
        }
    }
    if (control && frame.kind == FrameEvent::Kind::End) {
        const std::uint32_t limit = config_.max_control_frames_per_second;
        if (limit != 0 && ++control_in_window_ > limit) {
            return ProtocolFailure{CloseCode::PolicyViolation, "control frame rate limit exceeded"};
        }
    }
    return std::nullopt;
}

/// The incomplete-message timer (FRAGMENTATION.md section 1.2) is for FRAGMENTED messages: it
/// starts when the first fragment has been received completely and stops at FIN (or a discard).
/// A single frame that arrives slowly is covered by the idle timer, which counts complete frames.
void ProtocolSession::track_message(const FrameEvent& frame) {
    if (!assembler_.in_message()) {
        message_started_.reset();
    } else if (!message_started_ && frame.kind == FrameEvent::Kind::End) {
        message_started_ = now_;
    }
}

void ProtocolSession::tick(TimePoint now) {
    now_ = std::max(now_, now);
    if (state_ == SessionState::Closed) {
        return;
    }
    if (now_ - window_start_ >= std::chrono::seconds{1}) {
        window_start_ = now_;
        control_in_window_ = 0;
        data_in_window_ = 0;
    }
    run_timers();
}

void ProtocolSession::run_timers() {
    if (state_ == SessionState::Closing) {  // only the close timeout matters while closing
        if (elapsed(now_, closing_since_, config_.close_timeout)) {
            emit_closed(false);
        }
        return;
    }
    if (elapsed(now_, last_frame_, config_.idle_timeout)) {
        begin_close(static_cast<std::uint16_t>(CloseCode::GoingAway), "idle timeout");
    } else if (ping_sent_at_ && elapsed(now_, *ping_sent_at_, config_.pong_timeout)) {
        begin_close(static_cast<std::uint16_t>(CloseCode::GoingAway), "pong timeout");
    } else if (message_started_ &&
               elapsed(now_, *message_started_, config_.incomplete_message_timeout)) {
        if (const auto event = fail_connection(
                ProtocolFailure{CloseCode::PolicyViolation, "incomplete message timeout"})) {
            queue_.push(*event);
        }
    } else {
        heartbeat();
    }
}

/// A quiet connection is pinged (RFC 6455 section 5.5.2); one Ping at a time, counted from the
/// later of the last frame and the last Ping so that an active peer is never pinged.
void ProtocolSession::heartbeat() {
    if (ping_sent_at_ || !elapsed(now_, std::max(last_frame_, last_ping_), config_.ping_interval)) {
        return;
    }
    if (out_.queue_ping({}).has_value()) {
        ping_sent_at_ = now_;
        last_ping_ = now_;
    }
}

}  // namespace wsrv
