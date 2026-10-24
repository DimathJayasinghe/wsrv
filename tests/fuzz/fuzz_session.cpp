// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: ProtocolSession driven by an arbitrary script of operations. The input is a
// sequence of ops (low 3 bits of a byte select the op, the following bytes are its arguments):
//   0-3 feed the next 1..32 input bytes as client bytes   4 tick(+arg*250 ms)
//   5   send_text / send_binary / send_ping               6 close(code)
//   7   write part of the pending output (the transport accepted fewer bytes)
// Properties: never crashes; events: at most one Closed and nothing after it; the state only moves
// forward; the output parses as unmasked frames, holds at most one Close frame and nothing after
// it; pending output stays bounded; delivered text is valid UTF-8; after Closed nothing is
// written.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/session.hpp"
#include "protocol/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    return config;
}

void check_event(const wsrv::SessionEvent& event, bool& closed_seen) {
    WSRV_ASSERT(!closed_seen);  // nothing follows Closed
    if (event.kind == wsrv::SessionEvent::Kind::Closed) {
        closed_seen = true;
    }
    const bool data = event.kind == wsrv::SessionEvent::Kind::Message ||
                      event.kind == wsrv::SessionEvent::Kind::Part;
    if (data && event.type == wsrv::MessageType::Text) {
        WSRV_ASSERT(wsrv::is_valid_utf8(event.data));
        WSRV_ASSERT(wsrv::is_valid_utf8(event.tail));
    }
}

void drain_events(wsrv::ProtocolSession& session, wsrv::MutableByteView& input, bool& closed_seen) {
    while (const auto event = session.next_event(input)) {
        check_event(*event, closed_seen);
    }
}

/// The server's whole output must be a valid sequence of unmasked frames with a single, final
/// Close.
void check_output(wsrv::ByteView output) {
    wsrv::FrameParserConfig config;
    config.expect_masked = false;
    config.max_frame_payload = 1U << 20;
    wsrv::FrameParser parser{config};
    std::vector<std::byte> copy{output.begin(), output.end()};
    wsrv::MutableByteView input{copy};
    bool close_seen = false;
    for (;;) {
        const auto event = parser.next(input);
        WSRV_ASSERT(event.has_value());  // the output is always valid
        if (!event->has_value()) {
            return;
        }
        if ((*event)->kind == wsrv::FrameEvent::Kind::Header) {
            WSRV_ASSERT(!close_seen);  // nothing after the Close frame
            close_seen = (*event)->header.opcode == wsrv::Opcode::Close;
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    Script script{std::span<const std::uint8_t>{data + 1, size - 1}};
    const wsrv::SessionConfig config = make_config(data[0]);
    const std::size_t bound = config.outbound.max_frame_payload + wsrv::kMaxHeaderSize +
                              3 * (2 + wsrv::kMaxControlPayload);
    wsrv::TimePoint now;
    wsrv::ProtocolSession session{config, now};
    std::vector<std::byte> output;
    bool closed_seen = false;
    wsrv::SessionState last_state = session.state();

    while (!script.done()) {
        const std::uint8_t op = script.next();
        switch (op & 7U) {
        case 0:
        case 1:
        case 2:
        case 3: {
            const auto chunk = script.take(1 + script.next() % 32U);
            std::vector<std::byte> bytes(chunk.size());
            std::transform(chunk.begin(), chunk.end(), bytes.begin(), [](std::uint8_t b) {
                return static_cast<std::byte>(b);
            });
            wsrv::MutableByteView input{bytes};
            drain_events(session, input, closed_seen);
            WSRV_ASSERT(input.empty());
            break;
        }
        case 4:
            now += std::chrono::milliseconds{250} * script.next();
            session.tick(now);
            break;
        case 5: {
            const std::uint8_t kind = script.next();
            const auto body = script.take(script.next() % 40U);
            const wsrv::ByteView view = std::as_bytes(body);
            if (kind % 3 == 0) {
                (void)session.send_text(wsrv::as_chars(view));
            } else if (kind % 3 == 1) {
                (void)session.send_binary(view);
            } else {
                (void)session.send_ping(view);
            }
            break;
        }
        case 6:
            (void)session.close(static_cast<wsrv::CloseCode>(1000U + script.next() % 16U), "fuzz");
            break;
        default: {
            const wsrv::ByteView pending = session.pending_output();
            WSRV_ASSERT(pending.size() <= bound);
            const std::size_t take =
                std::min<std::size_t>(pending.size(), 1U + script.next() % 24U);
            output.insert(
                output.end(), pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(take));
            session.consume_output(take);
            break;
        }
        }
        wsrv::MutableByteView none;
        drain_events(session, none, closed_seen);  // Closed / errors produced by tick / flush
        WSRV_ASSERT(static_cast<int>(session.state()) >= static_cast<int>(last_state));
        last_state = session.state();
    }
    for (;;) {  // the transport finally writes everything
        const wsrv::ByteView pending = session.pending_output();
        if (pending.empty()) {
            break;
        }
        output.insert(output.end(), pending.begin(), pending.end());
        session.consume_output(pending.size());
    }
    if (session.state() == wsrv::SessionState::Closed) {
        WSRV_ASSERT(session.pending_output().empty());
    }
    check_output(output);
    return 0;
}
