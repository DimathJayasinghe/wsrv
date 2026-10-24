// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "base/error.hpp"
#include "protocol/frame.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/frame_writer.hpp"
#include "protocol/masker.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wsrv::test {

/// One frame as a peer sees it on the wire (payload already unmasked).
struct WireFrame {
    Opcode opcode = Opcode::Text;
    bool fin = true;
    std::string payload;

    friend bool operator==(const WireFrame&, const WireFrame&) = default;
};

/// Server output parsed the way a client would: unmasked frames only. `ok` is false if any byte
/// violates the protocol; `complete` is false if the bytes end inside a frame.
struct ParsedOutput {
    std::vector<WireFrame> frames;
    bool ok = true;
    bool complete = true;
};

inline constexpr MaskKey kTestMaskKey{
    std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};

/// A masked client->server frame (what a browser sends).
inline std::vector<std::byte> client_frame(Opcode opcode, bool fin, std::string_view payload) {
    FrameHeader header;
    header.fin = fin;
    header.opcode = opcode;
    header.masked = true;
    header.payload_length = payload.size();
    header.mask_key = kTestMaskKey;
    std::array<std::byte, kMaxHeaderSize> head{};
    const Result<std::size_t> encoded = encode_header(head, header);
    WSRV_ASSERT(encoded.has_value());  // a test asked for a frame that cannot exist (control > 125)
    std::vector<std::byte> out(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(*encoded));
    std::vector<std::byte> body(payload.size());
    std::transform(payload.begin(), payload.end(), body.begin(), [](char c) {
        return static_cast<std::byte>(c);
    });
    apply_mask(body, kTestMaskKey);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Close payload: big-endian code followed by the reason.
inline std::string close_payload(unsigned code, std::string_view reason = {}) {
    std::string out;
    out.push_back(static_cast<char>(code >> 8U));
    out.push_back(static_cast<char>(code & 0xFFU));
    out.append(reason);
    return out;
}

/// A masked control frame whose 16-bit length field says `length` (> 125): illegal on the wire
/// (RFC 6455 section 5.5), which encode_header refuses to produce, so it is built by hand.
inline std::vector<std::byte> oversized_control_frame(Opcode opcode, std::size_t length) {
    std::vector<std::byte> out{static_cast<std::byte>(0x80U | static_cast<unsigned>(opcode)),
                               std::byte{0xFE},
                               static_cast<std::byte>(length >> 8U),
                               static_cast<std::byte>(length & 0xFFU)};
    out.insert(out.end(), kTestMaskKey.begin(), kTestMaskKey.end());
    out.insert(out.end(), length, std::byte{0});
    return out;
}

inline ParsedOutput parse_server_output(ByteView bytes) {
    FrameParserConfig config;
    config.expect_masked = false;
    config.max_frame_payload = 1ULL << 30;
    FrameParser parser{config};
    std::vector<std::byte> copy{bytes.begin(), bytes.end()};
    MutableByteView input{copy};
    ParsedOutput out;
    for (;;) {
        const auto event = parser.next(input);
        if (!event) {
            out.ok = false;
            return out;
        }
        if (!event->has_value()) {
            break;
        }
        const FrameEvent& e = **event;
        if (e.kind == FrameEvent::Kind::Header) {
            out.frames.push_back(WireFrame{e.header.opcode, e.header.fin, {}});
        } else if (e.kind == FrameEvent::Kind::Payload) {
            out.frames.back().payload.append(as_chars(e.payload));
        }
        out.complete = e.kind == FrameEvent::Kind::End;
    }
    out.complete = out.complete || out.frames.empty();
    return out;
}

}  // namespace wsrv::test
