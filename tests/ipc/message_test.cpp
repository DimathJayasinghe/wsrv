// SPDX-License-Identifier: Apache-2.0
#include "ipc/message.hpp"

#include "base/byte_buffer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace wsrv;
using namespace wsrv::ipc;

constexpr std::size_t kLimit = 1U << 20;

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

std::vector<std::byte> encoded(const Message& message, std::size_t max_frame = kLimit) {
    ByteBuffer out{kLimit * 2};
    EXPECT_TRUE(encode(message, max_frame, out).has_value());
    const ByteView view = out.view();
    return {view.begin(), view.end()};
}

ByteView text(std::string_view s) {
    return as_bytes(s);
}

// ---- golden bytes: the format on the wire is part of the contract
// ---------------------------------

TEST(IpcMessage, PingIsSixBytes) {
    EXPECT_EQ(encoded(Ping{}), bytes_of({0, 0, 0, 2, 6, 0}));
    EXPECT_EQ(encoded(Pong{}), bytes_of({0, 0, 0, 2, 7, 0}));
}

TEST(IpcMessage, StatusLayout) {
    // length 2+5, type 9, flags 0, request 0x01020304, status 6
    EXPECT_EQ(encoded(Status{0x01020304U, 6}), bytes_of({0, 0, 0, 7, 9, 0, 1, 2, 3, 4, 6}));
}

TEST(IpcMessage, HelloLayout) {
    const std::vector<std::byte> token = bytes_of({'a', 'b'});
    const auto frame = encoded(Hello{0, 1, ByteView{token}});
    // length 2+6+2, type 1, flags 0, major 0, minor 1, token length 2, "ab"
    EXPECT_EQ(frame, bytes_of({0, 0, 0, 10, 1, 0, 0, 0, 0, 1, 0, 2, 'a', 'b'}));
}

TEST(IpcMessage, EventLayout) {
    const auto frame =
        encoded(Event{EventKind::Close, 0x0102030405060708ULL, 1001, true, text("bye")});
    EXPECT_EQ(frame, bytes_of({0,    0,    0,  2 + 12 + 3, 8, 0,        //
                               4,                                       // kind Close
                               1,    2,    3,  4,          5, 6, 7, 8,  // connection id
                               0x03, 0xE9,                              // 1001
                               1,                                       // clean
                               'b',  'y',  'e'}));
}

TEST(IpcMessage, HandshakeAnswersLayout) {  // ADR-0018: frames 10 and 11, event kind 6
    EXPECT_EQ(encoded(HandshakeAccept{0x01020304U, 0x0A0B0C0D0E0F1011ULL, text("v2")}),
              bytes_of({0,    0,    0,    2 + 12 + 2, 10,   0,                 //
                        1,    2,    3,    4,                                   // request
                        0x0A, 0x0B, 0x0C, 0x0D,       0x0E, 0x0F, 0x10, 0x11,  // connection id
                        'v',  '2'}));
    EXPECT_EQ(encoded(HandshakeAccept{1, 5, ByteView{}}),
              bytes_of({0, 0, 0, 2 + 12, 10, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 5}));
    EXPECT_EQ(encoded(HandshakeReject{9, 5, 403}),
              bytes_of({0, 0, 0, 2 + 14, 11, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 5, 0x01, 0x93}));
    const auto frame = encoded(Event{EventKind::Handshake, 5, 0, false, text("blob")});
    EXPECT_EQ(frame[6], std::byte{6});  // the kind byte
}

// ---- encode -> parse round trip for every message
// ---------------------------------------------------

Result<Message> reparse(const std::vector<std::byte>& frame) {
    const auto type = frame_type_from(std::to_integer<std::uint8_t>(frame.at(4)));
    EXPECT_TRUE(type.has_value());
    return parse_message(*type, ByteView{frame}.subspan(kFrameHeaderSize));
}

TEST(IpcMessage, EveryMessageSurvivesARoundTripByteForByte) {
    const std::vector<std::byte> token = bytes_of({1, 2, 3});
    const std::vector<Message> messages = {
        Hello{0, 1, ByteView{token}},
        Hello{0, 1, ByteView{}},
        HelloOk{0, 1, 123456},
        SendData{7, 42, true, text("héllo")},
        SendData{8, 42, false, ByteView{token}},
        SendData{9, 42, true, ByteView{}},
        CloseCommand{10, 42, 1000, text("done")},
        CloseCommand{11, 42, 4000, ByteView{}},
        Ping{},
        Pong{},
        Event{EventKind::Open, 5, 0, false, text("127.0.0.1:5555")},
        Event{EventKind::Text, 5, 0, false, text("hi")},
        Event{EventKind::Binary, 5, 0, false, ByteView{token}},
        Event{EventKind::Close, 5, 1006, false, ByteView{}},
        Event{EventKind::Error, 5, 0, false, text("Protocol")},
        Event{EventKind::Handshake, 5, 0, false, text("blob")},
        HandshakeAccept{12, 5, text("chat")},
        HandshakeAccept{13, 5, ByteView{}},
        HandshakeReject{14, 5, 401},
        TopicCommand{15, 5, true, text("room.1")},  // 0.3: broker frames (ADR-0028)
        TopicCommand{16, 5, false, text("room.1")},
        Publish{17, true, 0, text("room.1"), text("héllo")},
        Publish{18, false, 5, text("t"), ByteView{token}},
        Publish{19,
                true,
                0,
                ByteView{},
                ByteView{}},  // names are the daemon's to judge, not the codec's
        Status{1, 0},
        Status{0xFFFFFFFFU, 255},
    };
    for (const Message& message : messages) {
        const auto frame = encoded(message);
        const Result<Message> parsed = reparse(frame);
        ASSERT_TRUE(parsed.has_value()) << "index " << message.index();
        EXPECT_EQ(parsed->index(), message.index());
        EXPECT_EQ(encoded(*parsed), frame) << "index " << message.index();  // canonical form
    }
}

// ---- encode refusals
// --------------------------------------------------------------------------------

TEST(IpcMessage, EncodeRefusesAFrameAboveTheLimitAndLeavesTheBufferAlone) {
    ByteBuffer out{1U << 16};
    ASSERT_TRUE(encode(Ping{}, kLimit, out).has_value());
    const std::size_t before = out.size();
    const std::vector<std::byte> big(100, std::byte{1});
    const Result<void> r = encode(SendData{1, 1, false, ByteView{big}}, 50, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(out.size(), before);
}

TEST(IpcMessage, EncodeRefusesWhatTheOutputBufferCannotHold) {
    ByteBuffer out{8};
    const std::vector<std::byte> data(10, std::byte{1});
    const Result<void> r = encode(SendData{1, 1, false, ByteView{data}}, kLimit, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(out.size(), 0U);
}

TEST(IpcMessage, EncodeRefusesMessagesNoPeerCouldParse) {
    ByteBuffer out{1U << 16};
    const std::vector<std::byte> long_token(kMaxTokenSize + 1, std::byte{1});
    const std::vector<std::byte> long_reason(kMaxCloseReason + 1, std::byte{1});
    const std::vector<std::byte> long_subprotocol(kMaxSubprotocol + 1, std::byte{1});
    const std::vector<Message> bad = {
        Hello{0, 1, ByteView{long_token}},
        CloseCommand{1, 1, 1000, ByteView{long_reason}},
        SendData{1, 0, true, ByteView{}},                           // connection 0
        CloseCommand{1, 0, 1000, ByteView{}},                       // connection 0
        Event{EventKind::Open, 0, 0, false, ByteView{}},            // connection 0
        Event{EventKind::Text, 1, 1000, false, ByteView{}},         // close code on a Text
        Event{EventKind::Text, 1, 0, true, ByteView{}},             // clean on a Text
        Event{static_cast<EventKind>(0), 1, 0, false, ByteView{}},  // unknown kind
        Event{static_cast<EventKind>(7), 1, 0, false, ByteView{}},
        Event{EventKind::Handshake, 1, 1000, false, ByteView{}},  // close code on a Handshake
        HandshakeAccept{1, 0, ByteView{}},                        // connection 0
        HandshakeAccept{1, 1, ByteView{long_subprotocol}},
        HandshakeReject{1, 0, 403},            // connection 0
        TopicCommand{1, 0, true, text("t")},   // connection 0
        TopicCommand{1, 0, false, text("t")},  // connection 0
    };
    for (const Message& message : bad) {
        const Result<void> r = encode(message, kLimit, out);
        ASSERT_FALSE(r.has_value()) << "index " << message.index();
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(out.size(), 0U);
}

// ---- parse refusals: every shape of a wrong body is InvalidFrame
// ------------------------------------

struct BadBody {
    FrameType type;
    std::vector<std::byte> body;
    const char* why;
};

TEST(IpcMessage, ParseRefusesWrongSizesAndImpossibleFields) {
    const std::vector<BadBody> cases = {
        {FrameType::Ping, bytes_of({0}), "ping with a body"},
        {FrameType::Pong, bytes_of({0}), "pong with a body"},
        {FrameType::Hello, bytes_of({0, 0, 0, 1}), "hello without token length"},
        {FrameType::Hello, bytes_of({0, 0, 0, 1, 0, 3, 'a'}), "token shorter than declared"},
        {FrameType::Hello, bytes_of({0, 0, 0, 1, 0, 0, 'a'}), "token longer than declared"},
        {FrameType::HelloOk, bytes_of({0, 0, 0, 1, 0, 0, 0}), "hello_ok too short"},
        {FrameType::HelloOk, bytes_of({0, 0, 0, 1, 0, 0, 0, 1, 9}), "hello_ok trailing byte"},
        {FrameType::SendText, bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}), "send_text too short"},
        {FrameType::SendText, bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}), "send_text conn 0"},
        {FrameType::SendBinary, bytes_of({0, 0, 0, 1}), "send_binary far too short"},
        {FrameType::Close,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 3}),
         "close without full code"},
        {FrameType::Close, bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232}), "close conn 0"},
        {FrameType::Event, bytes_of({1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}), "event too short"},
        {FrameType::Event, bytes_of({0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}), "event kind 0"},
        {FrameType::Event, bytes_of({7, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}), "event kind 7"},
        {FrameType::Event,
         bytes_of({6, 0, 0, 0, 0, 0, 0, 0, 1, 3, 232, 0}),
         "close code on Handshake"},
        {FrameType::HandshakeAccept,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}),
         "accept too short"},
        {FrameType::HandshakeAccept,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}),
         "accept conn 0"},
        {FrameType::HandshakeReject,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 5, 1}),
         "reject too short"},
        {FrameType::HandshakeReject,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 5, 1, 147, 0}),
         "reject trailing byte"},
        {FrameType::HandshakeReject,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 147}),
         "reject conn 0"},
        {FrameType::Event, bytes_of({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), "event conn 0"},
        {FrameType::Event, bytes_of({4, 0, 0, 0, 0, 0, 0, 0, 1, 3, 232, 2}), "clean flag 2"},
        {FrameType::Event, bytes_of({2, 0, 0, 0, 0, 0, 0, 0, 1, 3, 232, 0}), "close code on Text"},
        {FrameType::Event, bytes_of({2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1}), "clean on Text"},
        {FrameType::Status, bytes_of({0, 0, 0, 1}), "status too short"},
        {FrameType::Status, bytes_of({0, 0, 0, 1, 0, 0}), "status trailing byte"},
    };
    for (const BadBody& c : cases) {
        const Result<Message> r = parse_message(c.type, ByteView{c.body});
        ASSERT_FALSE(r.has_value()) << c.why;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidFrame) << c.why;
    }
}

TEST(IpcMessage, ParseRefusesATokenOrReasonOverTheCap) {
    std::vector<std::byte> hello = bytes_of({0, 0, 0, 1, 0x01, 0x01});  // token length 257
    hello.insert(hello.end(), kMaxTokenSize + 1, std::byte{'t'});
    EXPECT_EQ(parse_message(FrameType::Hello, ByteView{hello}).error().code,
              ErrorCode::InvalidFrame);

    std::vector<std::byte> close = bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 3, 232});
    close.insert(close.end(), kMaxCloseReason + 1, std::byte{'r'});
    EXPECT_EQ(parse_message(FrameType::Close, ByteView{close}).error().code,
              ErrorCode::InvalidFrame);

    std::vector<std::byte> accept = bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 5});
    accept.insert(accept.end(), kMaxSubprotocol + 1, std::byte{'p'});
    EXPECT_EQ(parse_message(FrameType::HandshakeAccept, ByteView{accept}).error().code,
              ErrorCode::InvalidFrame);
    accept.pop_back();  // exactly at the cap is fine
    EXPECT_TRUE(parse_message(FrameType::HandshakeAccept, ByteView{accept}).has_value());
}

TEST(IpcMessage, BrokerFramesLayout) {  // ADR-0028 section 2: frames 12, 13, 14; minor 3
    EXPECT_EQ(kVersionMinor, 3U);
    // length 2+12+6, type 12, request 7, conn 42, topic "room.1"
    EXPECT_EQ(encoded(TopicCommand{7, 42, true, text("room.1")}),
              bytes_of({0, 0, 0, 20, 12, 0,  0,   0,   0,   7,   0,   0,
                        0, 0, 0, 0,  0,  42, 'r', 'o', 'o', 'm', '.', '1'}));
    EXPECT_EQ(encoded(TopicCommand{7, 42, false, text("room.1")}).at(4), std::byte{13});
    // length 2+15+1+2, type 14, request 9, kind 1 (text), except 5, topic length 1, "t", "hi"
    EXPECT_EQ(
        encoded(Publish{9, true, 5, text("t"), text("hi")}),
        bytes_of({0, 0, 0, 20, 14, 0, 0, 0, 0, 9, 1, 0, 0, 0, 0, 0, 0, 0, 5, 0, 1, 't', 'h', 'i'}));
    EXPECT_EQ(encoded(Publish{9, false, 5, text("t"), text("hi")}).at(10), std::byte{2});
}

TEST(IpcMessage, ParseRefusesWrongBrokerFrames) {
    const std::vector<BadBody> cases = {
        {FrameType::Subscribe, bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}), "subscribe too short"},
        {FrameType::Subscribe,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 't'}),
         "subscribe conn 0"},
        {FrameType::Unsubscribe, bytes_of({0, 0, 0, 1}), "unsubscribe far too short"},
        {FrameType::Publish,
         bytes_of({0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
         "publish without a topic length"},
        {FrameType::Publish,
         bytes_of({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
         "publish kind 0"},
        {FrameType::Publish,
         bytes_of({0, 0, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
         "publish kind 3"},
        {FrameType::Publish,
         bytes_of({0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 'a'}),
         "topic longer than the frame"},
    };
    for (const BadBody& c : cases) {
        const Result<Message> r = parse_message(c.type, ByteView{c.body});
        ASSERT_FALSE(r.has_value()) << c.why;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidFrame) << c.why;
    }
}

TEST(IpcMessage, UnknownFrameTypesAreRejectedByTheLookup) {
    EXPECT_FALSE(frame_type_from(0).has_value());
    EXPECT_FALSE(frame_type_from(15).has_value());
    EXPECT_FALSE(frame_type_from(255).has_value());
    for (std::uint8_t raw = 1; raw <= 14; ++raw) {
        EXPECT_TRUE(frame_type_from(raw).has_value()) << int{raw};
    }
}

}  // namespace
