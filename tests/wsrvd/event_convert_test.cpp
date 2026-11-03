// SPDX-License-Identifier: Apache-2.0
// The core emits HostEvents; the IPC encoder refuses events whose fields are inconsistent. If the
// two ever disagree, the pump would fail to encode a legal event and drop the whole IPC link. This
// test walks every shape the core can produce and checks it survives encode and parse.
#include "wsrvd/event_convert.hpp"

#include "base/byte_buffer.hpp"
#include "ipc/message.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <variant>

namespace {

using wsrv::ByteBuffer;
using wsrv::HostEvent;
using wsrv::HostEventType;
using wsrv::ipc::Event;
using wsrv::ipc::EventKind;
using wsrv::ipc::FrameType;
using wsrv::wsrvd::to_ipc_event;

constexpr std::size_t kMaxFrame = 4096;

HostEvent host_event(HostEventType type, std::string_view text = {}) {
    HostEvent event;
    event.type = type;
    event.conn = 7;
    for (const char c : text) {
        event.data.push_back(static_cast<std::byte>(c));
    }
    return event;
}

/// Encode `event`, decode it again and return what the application would see.
Event round_trip(const HostEvent& event, ByteBuffer& storage) {
    const wsrv::ipc::Message message = to_ipc_event(event);
    const auto encoded = wsrv::ipc::encode(message, kMaxFrame, storage);
    EXPECT_TRUE(encoded.has_value()) << "the encoder refused an event the core can emit";
    const wsrv::ByteView frame = storage.view();
    // Skip the 6-byte header (length, type, flags); parse_message takes the body.
    const auto parsed = wsrv::ipc::parse_message(FrameType::Event, frame.subspan(6));
    EXPECT_TRUE(parsed.has_value()) << "the parser refused what the encoder wrote";
    const auto* out = std::get_if<Event>(&*parsed);
    EXPECT_NE(out, nullptr);
    return *out;
}

TEST(EventConvert, KindsKeepTheirWireNumbers) {
    ByteBuffer storage{kMaxFrame};
    EXPECT_EQ(round_trip(host_event(HostEventType::Open, "10.0.0.1:5000"), storage).kind,
              EventKind::Open);
}

TEST(EventConvert, EveryNonCloseShapeEncodes) {
    constexpr std::array types{HostEventType::Open,
                               HostEventType::Text,
                               HostEventType::Binary,
                               HostEventType::Error};
    for (const HostEventType type : types) {
        ByteBuffer storage{kMaxFrame};
        const Event out = round_trip(host_event(type, "abc"), storage);
        EXPECT_EQ(out.conn, 7U);
        EXPECT_EQ(out.close_code, 0U);
        EXPECT_FALSE(out.clean);
        EXPECT_EQ(out.data.size(), 3U);
    }
}

TEST(EventConvert, CloseKeepsCodeCleanFlagAndReason) {
    // 1005 (no status) and 1006 (abnormal) never appear on the wire in RFC 6455, but the core
    // reports them to the host, so the IPC link must carry them (RFC 6455 section 7.1.5, 7.1.6).
    constexpr std::array<std::uint16_t, 5> codes{1000, 1001, 1005, 1006, 1012};
    for (const std::uint16_t code : codes) {
        for (const bool clean : {true, false}) {
            HostEvent event = host_event(HostEventType::Close, "bye");
            event.close_code = code;
            event.clean = clean;
            ByteBuffer storage{kMaxFrame};
            const Event out = round_trip(event, storage);
            EXPECT_EQ(out.kind, EventKind::Close);
            EXPECT_EQ(out.close_code, code);
            EXPECT_EQ(out.clean, clean);
        }
    }
}

TEST(EventConvert, ACloseReasonAtTheCoreLimitStillFits) {
    HostEvent event = host_event(HostEventType::Close, std::string(wsrv::ipc::kMaxCloseReason, 'r'));
    event.close_code = 1000;
    event.clean = true;
    ByteBuffer storage{kMaxFrame};
    EXPECT_EQ(round_trip(event, storage).data.size(), wsrv::ipc::kMaxCloseReason);
}

TEST(EventConvert, FrameSizeIsPayloadPlusTheFixedOverhead) {
    // The pump relies on this to know whether an event can ever fit the configured frame limit.
    EXPECT_EQ(wsrv::wsrvd::encoded_event_size(host_event(HostEventType::Text, "abcd")),
              wsrv::ipc::kFrameHeaderSize + wsrv::wsrvd::kEventBodyOverhead + 4U);
    ByteBuffer storage{kMaxFrame};
    ASSERT_TRUE(wsrv::ipc::encode(to_ipc_event(host_event(HostEventType::Text, "abcd")),
                                  kMaxFrame,
                                  storage)
                    .has_value());
    EXPECT_EQ(storage.view().size(),
              wsrv::wsrvd::encoded_event_size(host_event(HostEventType::Text, "abcd")));
}

}  // namespace
