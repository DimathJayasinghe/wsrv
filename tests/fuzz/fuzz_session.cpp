// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: ProtocolSession driven by an arbitrary script of operations. The input is a
// sequence of ops (low 3 bits of a byte select the op, the following bytes are its arguments):
//   0-2 feed the next 1..32 input bytes as client bytes   3   feed one VALID masked frame
//   4   tick(+arg*250 ms)                                 5   send_text / send_binary / send_ping,
//                                                             begin_message / write_part /
//                                                             end_message
//   6   close(code)                                       7   write part of the pending output,
//                                                             or (odd arg) the transport ends
// Properties: never crashes; events: at most one Closed and nothing after it, at most one
// ProtocolError, no message/Ping/Pong once the session is no longer Open; Closed is reported after
// the transport ends; the state only moves forward; the output parses as unmasked frames, holds at
// most one Close frame and nothing after it, data frames follow the fragmentation grammar
// (RFC 6455 section 5.4: a message starts with Text/Binary, continues with Continuation until FIN,
// never interleaves another data message); pending output stays bounded; delivered text is valid
// UTF-8; after Closed nothing is written.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#if defined(WSRV_FUZZ_DEFLATE)
#include "extensions/zlib_codec.hpp"
#endif
#include "protocol/frame.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/frame_writer.hpp"
#include "protocol/masker.hpp"
#include "protocol/session.hpp"
#include "protocol/utf8.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace std::chrono_literals;

class Script {
public:
    explicit Script(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}
    [[nodiscard]] bool done() const {
        return pos_ >= bytes_.size();
    }
    std::uint8_t next() {
        return done() ? std::uint8_t{0} : bytes_[pos_++];
    }
    std::span<const std::uint8_t> take(std::size_t count) {
        count = std::min(count, bytes_.size() - pos_);
        const auto chunk = bytes_.subspan(pos_, count);
        pos_ += count;
        return chunk;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_ = 0;
};

wsrv::SessionConfig make_config(std::uint8_t selector) {
    wsrv::SessionConfig config;
    config.assembler.streaming = (selector & 1U) != 0;
    config.assembler.max_message_size = std::size_t{16} << ((selector >> 1U) & 3U);  // 16..128
    config.assembler.max_fragments = 4;
    config.parser.max_frame_payload = 256;
    config.outbound.max_frame_payload = 8U + ((selector >> 3U) & 3U) * 8U;
    config.outbound.max_queued_bytes = 256;
    config.ping_interval = 1000ms;
    config.pong_timeout = 500ms;
    config.idle_timeout = 5000ms;
    config.close_timeout = 750ms;
    config.incomplete_message_timeout = 2000ms;
    config.max_control_frames_per_second = 6;
    config.max_data_frames_per_second = 12;
#if defined(WSRV_FUZZ_DEFLATE)
    if ((selector & 32U) != 0) {  // permessage-deflate negotiated (ADR-0020)
        wsrv::SessionDeflate deflate;
        deflate.codecs = std::make_shared<wsrv::ZlibCodecFactory>(wsrv::ZlibSettings{});
        deflate.min_compress_size = 8;
        deflate.max_inflate_ratio = 20;
        deflate.params.server_no_context_takeover = (selector & 64U) != 0;
        config.deflate = deflate;
    }
#endif
    return config;
}

/// What the fuzz run has seen of the session's events so far.
struct Watch {
    bool closed = false;
    int protocol_errors = 0;
};

void check_event(const wsrv::SessionEvent& event, wsrv::SessionState state, Watch& watch) {
    WSRV_ASSERT(!watch.closed);  // nothing follows Closed
    using Kind = wsrv::SessionEvent::Kind;
    if (event.kind == Kind::Closed) {
        watch.closed = true;
    } else if (event.kind == Kind::ProtocolError) {
        WSRV_ASSERT(++watch.protocol_errors == 1);
    } else {
        WSRV_ASSERT(state == wsrv::SessionState::Open);  // nothing is delivered once closing
    }
    const bool data = event.kind == Kind::Message || event.kind == Kind::Part;
    if (data && event.type == wsrv::MessageType::Text) {
        WSRV_ASSERT(wsrv::is_valid_utf8(event.data));
        WSRV_ASSERT(wsrv::is_valid_utf8(event.tail));
    }
}

void drain_events(wsrv::ProtocolSession& session, wsrv::MutableByteView& input, Watch& watch) {
    while (const auto event = session.next_event(input)) {
        check_event(*event, session.state(), watch);
    }
}

/// One well-formed masked client frame built from script bytes: the fuzzer cannot easily invent the
/// 4-byte masks and lengths of valid frames, so this is how it reaches Pong, Continuation and
/// Close.
std::vector<std::byte> valid_frame(Script& script) {
    static constexpr std::array kOpcodes{wsrv::Opcode::Text,
                                         wsrv::Opcode::Binary,
                                         wsrv::Opcode::Continuation,
                                         wsrv::Opcode::Ping,
                                         wsrv::Opcode::Pong,
                                         wsrv::Opcode::Close,
                                         wsrv::Opcode::Continuation,
                                         wsrv::Opcode::Text};
    const std::uint8_t selector = script.next();
    wsrv::FrameHeader header;
    header.opcode = kOpcodes[selector & 7U];
    header.fin = (selector & 8U) == 0 || wsrv::is_control(header.opcode);
    header.masked = true;
    header.rsv1 = (selector & 64U) != 0 && !wsrv::is_control(header.opcode) &&
                  header.opcode != wsrv::Opcode::Continuation;  // compressed, if negotiated
    header.mask_key = wsrv::MaskKey{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::vector<std::byte> body;
    if (header.opcode == wsrv::Opcode::Close && (selector & 16U) == 0) {
        body = {std::byte{0x03}, std::byte{0xE8}};  // a valid status code
    } else {
        for (const std::uint8_t b : script.take(script.next() % 20U)) {
            body.push_back(static_cast<std::byte>(b));
        }
    }
    if ((selector & 32U) != 0 && header.opcode != wsrv::Opcode::Binary &&
        !wsrv::is_control(header.opcode)) {
        body = {std::byte{'h'}, std::byte{0xC3}, std::byte{0xA9}};  // valid UTF-8 text
    }
    header.payload_length = body.size();
    std::array<std::byte, wsrv::kMaxHeaderSize> head{};
    const auto size = wsrv::encode_header(head, header);
    WSRV_ASSERT(size.has_value());  // control payloads above are at most 19 bytes
    wsrv::apply_mask(body, header.mask_key);
    std::vector<std::byte> frame(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(*size));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

/// The server's whole output must be a valid sequence of unmasked frames with a single, final
/// Close.
void check_output(wsrv::ByteView output) {
    wsrv::FrameParserConfig config;
    config.expect_masked = false;
    config.allow_rsv1 = true;  // compressed messages carry RSV1 on their first frame
    config.max_frame_payload = 1U << 20;
    wsrv::FrameParser parser{config};
    std::vector<std::byte> copy{output.begin(), output.end()};
    wsrv::MutableByteView input{copy};
    bool close_seen = false;
    bool in_message = false;  // a data message without FIN is on the wire
    for (;;) {
        const auto event = parser.next(input);
        WSRV_ASSERT(event.has_value());  // the output is always valid
        if (!event->has_value()) {
            return;
        }
        if ((*event)->kind == wsrv::FrameEvent::Kind::Header) {
            WSRV_ASSERT(!close_seen);  // nothing after the Close frame
            const wsrv::FrameHeader& header = (*event)->header;
            close_seen = header.opcode == wsrv::Opcode::Close;
            if (!wsrv::is_control(header.opcode)) {
                WSRV_ASSERT((header.opcode == wsrv::Opcode::Continuation) == in_message);
                in_message = !header.fin;
            }
        }
    }
}

}  // namespace

namespace {

/// The transport accepted `take` bytes of the pending output (the fuzzer controls how many).
void write_some(wsrv::ProtocolSession& session,
                std::vector<std::byte>& output,
                std::size_t limit,
                std::size_t bound) {
    const wsrv::ByteView pending = session.pending_output();
    WSRV_ASSERT(pending.size() <= bound);
    const std::size_t take = std::min(pending.size(), limit);
    output.insert(
        output.end(), pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(take));
    session.consume_output(take);
}

void feed(wsrv::ProtocolSession& session, std::vector<std::byte> bytes, Watch& watch) {
    wsrv::MutableByteView input{bytes};
    drain_events(session, input, watch);
    WSRV_ASSERT(input.empty());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    Script script{std::span<const std::uint8_t>{data + 1, size - 1}};
    const wsrv::SessionConfig config = make_config(data[0]);
    WSRV_ASSERT(wsrv::validate(config).has_value());
    const std::size_t bound =
        std::max(config.outbound.max_frame_payload + wsrv::kMaxHeaderSize,
                 2 + wsrv::kMaxControlPayload);  // the wire buffer holds exactly one frame
    wsrv::TimePoint now;
    wsrv::ProtocolSession session{config, now};
    std::vector<std::byte> output;
    Watch watch;
    wsrv::SessionState last_state = session.state();

    while (!script.done()) {
        switch (script.next() & 7U) {
        case 0:
        case 1:
        case 2: {
            const auto chunk = script.take(1 + script.next() % 32U);
            feed(session,
                 std::vector<std::byte>{std::as_bytes(chunk).begin(), std::as_bytes(chunk).end()},
                 watch);
            break;
        }
        case 3:
            feed(session, valid_frame(script), watch);
            break;
        case 4:
            now += std::chrono::milliseconds{250} * script.next();
            session.tick(now);
            break;
        case 5: {
            const std::uint8_t kind = script.next();
            const wsrv::ByteView view = std::as_bytes(script.take(script.next() % 40U));
            switch (kind % 6U) {
            case 0:
                (void)session.send_text(wsrv::as_chars(view));
                break;
            case 1:
                (void)session.send_binary(view);
                break;
            case 2:
                (void)session.send_ping(view);
                break;
            case 3:
                (void)session.begin_message(kind % 2U == 0 ? wsrv::Opcode::Text
                                                           : wsrv::Opcode::Binary);
                break;
            case 4:
                (void)session.write_part(view);
                break;
            default:
                (void)session.end_message();
                break;
            }
            break;
        }
        case 6:
            (void)session.close(static_cast<wsrv::CloseCode>(1000U + script.next() % 16U), "fuzz");
            break;
        default: {
            const std::uint8_t arg = script.next();
            if ((arg & 1U) == 0) {
                write_some(session, output, 1U + arg / 2U % 24U, bound);
            } else {
                session.on_transport_closed();
                wsrv::MutableByteView none;
                drain_events(session, none, watch);
                WSRV_ASSERT(watch.closed);  // the end of the transport is always reported
            }
            break;
        }
        }
        wsrv::MutableByteView none;
        drain_events(session, none, watch);  // Closed / errors produced by tick / flush
        WSRV_ASSERT(static_cast<int>(session.state()) >= static_cast<int>(last_state));
        last_state = session.state();
    }
    while (session.state() != wsrv::SessionState::Closed && !session.pending_output().empty()) {
        write_some(session, output, bound, bound);  // the transport finally writes everything
    }
    if (session.state() == wsrv::SessionState::Closed) {
        WSRV_ASSERT(session.pending_output().empty());
    }
    check_output(output);
    return 0;
}
