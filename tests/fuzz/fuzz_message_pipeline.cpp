// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: bytes -> FrameParser -> MessageAssembler. Properties: chunking never changes
// the outcome; buffered and streaming delivery produce the same messages, control frames and
// failure; delivered text is valid UTF-8; no message exceeds the configured size. Input layout:
// byte 0 selects limits, the rest is the byte stream.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "protocol/frame_parser.hpp"
#include "protocol/message_assembler.hpp"
#include "protocol/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct Item {
    bool is_control = false;
    wsrv::MessageType type = wsrv::MessageType::Text;
    wsrv::Opcode control = wsrv::Opcode::Ping;
    std::string data;

    friend bool operator==(const Item&, const Item&) = default;
};

struct Outcome {
    std::vector<Item> items;
    std::optional<wsrv::CloseCode> failure;

    friend bool operator==(const Outcome&, const Outcome&) = default;
};

Outcome
run(wsrv::ByteView data, const wsrv::AssemblerConfig& config, std::span<const std::uint8_t> sizes) {
    wsrv::FrameParser parser{};
    wsrv::MessageAssembler assembler{config};
    Outcome out;
    std::string streamed;  // parts of the message being streamed
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
            const auto frame_event = parser.next(input);
            if (!frame_event) {
                out.failure = frame_event.error().code;
                return out;
            }
            if (!frame_event->has_value()) {
                break;
            }
            const auto message_event = assembler.handle(**frame_event);
            if (!message_event) {
                out.failure = message_event.error().code;
                return out;
            }
            if (!message_event->has_value()) {
                continue;
            }
            const wsrv::MessageEvent& e = **message_event;
            const std::string text =
                std::string{wsrv::as_chars(e.data)} + std::string{wsrv::as_chars(e.tail)};
            if (e.kind == wsrv::MessageEvent::Kind::Control) {
                out.items.push_back(Item{true, wsrv::MessageType::Text, e.control_opcode, text});
            } else if (e.kind == wsrv::MessageEvent::Kind::Message) {
                out.items.push_back(Item{false, e.type, wsrv::Opcode::Ping, text});
            } else {
                streamed += text;  // a Part
                if (e.type == wsrv::MessageType::Text) {
                    WSRV_ASSERT(wsrv::is_valid_utf8(e.data));  // every part decodes on its own
                    WSRV_ASSERT(wsrv::is_valid_utf8(e.tail));
                }
                WSRV_ASSERT(e.tail.empty() || e.data.size() <= 4);  // tail follows a lone character
                if (e.final) {
                    out.items.push_back(Item{false, e.type, wsrv::Opcode::Ping, streamed});
                    streamed.clear();
                }
            }
        }
    }
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    wsrv::AssemblerConfig buffered;
    buffered.max_message_size = std::size_t{8} << (data[0] & 7U);       // 8 .. 1024
    buffered.max_fragments = std::size_t{2} << ((data[0] >> 3U) & 3U);  // 2 .. 16
    wsrv::AssemblerConfig streaming = buffered;
    streaming.streaming = true;
    const std::span<const std::uint8_t> raw{data + 1, size - 1};
    const wsrv::ByteView bytes = std::as_bytes(raw);

    const Outcome whole = run(bytes, buffered, {});
    const std::uint8_t one = 0;
    WSRV_ASSERT(whole == run(bytes, buffered, std::span<const std::uint8_t>{&one, 1}));
    WSRV_ASSERT(whole == run(bytes, buffered, raw));

    // The two delivery modes are independent code paths and must agree on what the peer sent.
    const Outcome streamed = run(bytes, streaming, {});
    WSRV_ASSERT(whole == streamed);
    WSRV_ASSERT(streamed == run(bytes, streaming, std::span<const std::uint8_t>{&one, 1}));
    WSRV_ASSERT(streamed == run(bytes, streaming, raw));

    for (const Item& item : whole.items) {
        if (!item.is_control) {
            WSRV_ASSERT(item.data.size() <= buffered.max_message_size);
            if (item.type == wsrv::MessageType::Text) {
                WSRV_ASSERT(wsrv::is_valid_utf8(wsrv::as_bytes(item.data)));
            }
        }
    }
    return 0;
}
