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
    const std::vector<Message> bad = {
        Hello{0, 1, ByteView{long_token}},
        CloseCommand{1, 1, 1000, ByteView{long_reason}},
        SendData{1, 0, true, ByteView{}},                           // connection 0
        CloseCommand{1, 0, 1000, ByteView{}},                       // connection 0
        Event{EventKind::Open, 0, 0, false, ByteView{}},            // connection 0
        Event{EventKind::Text, 1, 1000, false, ByteView{}},         // close code on a Text
        Event{EventKind::Text, 1, 0, true, ByteView{}},             // clean on a Text
        Event{static_cast<EventKind>(0), 1, 0, false, ByteView{}},  // unknown kind
        Event{static_cast<EventKind>(6), 1, 0, false, ByteView{}},
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
        {FrameType::Event, bytes_of({6, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}), "event kind 6"},
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
}

TEST(IpcMessage, UnknownFrameTypesAreRejectedByTheLookup) {
    EXPECT_FALSE(frame_type_from(0).has_value());
    EXPECT_FALSE(frame_type_from(10).has_value());
    EXPECT_FALSE(frame_type_from(255).has_value());
    for (std::uint8_t raw = 1; raw <= 9; ++raw) {
        EXPECT_TRUE(frame_type_from(raw).has_value()) << int{raw};
    }
}

}  // namespace
