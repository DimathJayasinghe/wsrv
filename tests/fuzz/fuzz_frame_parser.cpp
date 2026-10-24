// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the frame parser must give identical results however the bytes are chunked,
// deliver exactly the declared payload for every completed frame, and only ever accept headers that
// obey RFC 6455 section 5. Input layout: byte 0 selects the configuration, the rest is the byte
// stream.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "protocol/frame_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct Frame {
    wsrv::FrameHeader header;
    std::string payload;
    bool complete = false;

    friend bool operator==(const Frame&, const Frame&) = default;
};

struct Outcome {
    std::vector<Frame> frames;
    std::optional<wsrv::CloseCode> failure;

    friend bool operator==(const Outcome&, const Outcome&) = default;
};

wsrv::FrameParserConfig config_from(std::uint8_t selector) {
    wsrv::FrameParserConfig config;
    config.expect_masked = (selector & 1U) != 0;
    config.allow_rsv1 = (selector & 2U) != 0;
    config.max_frame_payload = std::uint64_t{16} << (2U * ((selector >> 2U) & 7U));  // 16 .. 262144
    return config;
}

/// Chunk sizes come from `sizes` (cycled, 1..16); an empty `sizes` means one chunk.
Outcome run(wsrv::ByteView data,
            const wsrv::FrameParserConfig& config,
            std::span<const std::uint8_t> sizes) {
    wsrv::FrameParser parser{config};
    Outcome out;
    std::size_t pos = 0;
    std::size_t index = 0;
    while (pos < data.size()) {
        std::size_t take = data.size() - pos;
        if (!sizes.empty()) {
            take = std::min(take, std::size_t{1} + sizes[index++ % sizes.size()] % 16U);
        }
        std::vector<std::byte> buffer(data.begin() + static_cast<std::ptrdiff_t>(pos),
                                      data.begin() + static_cast<std::ptrdiff_t>(pos + take));
        wsrv::MutableByteView input{buffer};
        pos += take;
        for (;;) {
            const auto event = parser.next(input);
            if (!event) {
                out.failure = event.error().code;
                return out;
            }
            if (!event->has_value()) {
                break;
            }
            switch ((*event)->kind) {
            case wsrv::FrameEvent::Kind::Header:
                out.frames.push_back(Frame{(*event)->header, {}, false});
                break;
            case wsrv::FrameEvent::Kind::Payload:
                WSRV_ASSERT(!out.frames.empty() && !out.frames.back().complete);
                out.frames.back().payload += std::string{wsrv::as_chars((*event)->payload)};
                break;
            case wsrv::FrameEvent::Kind::End:
                WSRV_ASSERT(!out.frames.empty());
                out.frames.back().complete = true;
                WSRV_ASSERT(out.frames.back().payload.size() ==
                            out.frames.back().header.payload_length);
                break;
            }
        }
    }
    return out;
}

void check_header(const wsrv::FrameHeader& h, const wsrv::FrameParserConfig& config) {
    WSRV_ASSERT(!h.rsv2 && !h.rsv3);
    WSRV_ASSERT(!h.rsv1 || (config.allow_rsv1 && !wsrv::is_control(h.opcode) &&
                            h.opcode != wsrv::Opcode::Continuation));
    WSRV_ASSERT(h.masked == config.expect_masked);
    WSRV_ASSERT(h.payload_length <= config.max_frame_payload && h.payload_length < (1ULL << 63));
    if (wsrv::is_control(h.opcode)) {
        WSRV_ASSERT(h.fin && h.payload_length <= wsrv::kMaxControlPayload);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const wsrv::FrameParserConfig config = config_from(data[0]);
    const std::span<const std::uint8_t> raw{data + 1, size - 1};
    const wsrv::ByteView bytes = std::as_bytes(raw);

    const Outcome whole = run(bytes, config, {});
    const std::uint8_t one = 0;
    WSRV_ASSERT(whole ==
                run(bytes, config, std::span<const std::uint8_t>{&one, 1}));  // byte at a time
    WSRV_ASSERT(whole == run(bytes, config, raw));  // data-derived chunking

    WSRV_ASSERT(!whole.failure || *whole.failure == wsrv::CloseCode::ProtocolError ||
                *whole.failure == wsrv::CloseCode::MessageTooBig);
    for (const Frame& frame : whole.frames) {
        check_header(frame.header, config);
        WSRV_ASSERT(frame.payload.size() <= frame.header.payload_length);
    }
    return 0;
}
