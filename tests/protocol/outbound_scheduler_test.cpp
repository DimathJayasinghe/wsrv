// SPDX-License-Identifier: Apache-2.0
#include "protocol/outbound_scheduler.hpp"

#include "support/wire.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using wsrv::ErrorCode;
using wsrv::Opcode;
using wsrv::OutboundConfig;
using wsrv::OutboundScheduler;
using wsrv::test::parse_server_output;
using wsrv::test::WireFrame;

wsrv::ByteView bytes_of(const std::string& s) {
    return wsrv::as_bytes(std::string_view{s});
}

OutboundConfig config(std::size_t frame = 64, std::size_t queue = 1U << 20) {
    OutboundConfig c;
    c.max_frame_payload = frame;
    c.max_queued_bytes = queue;
    return c;
}

/// Drain everything the scheduler wants to write, `step` bytes at a time (0 = whatever it offers).
std::vector<std::byte> drain(OutboundScheduler& s, std::size_t step = 0) {
    std::vector<std::byte> out;
    for (;;) {
        const wsrv::ByteView view = s.pending();
        if (view.empty()) {
            return out;
        }
        const std::size_t take = step == 0 ? view.size() : std::min(step, view.size());
        out.insert(out.end(), view.begin(), view.begin() + static_cast<std::ptrdiff_t>(take));
        s.consume(take);
    }
}

TEST(OutboundScheduler, ASmallMessageIsOneUnmaskedFinFrame) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("Hello")).has_value());
    const std::vector<std::byte> out = drain(s);
    const std::vector<std::byte> expected{std::byte{0x81},
                                          std::byte{0x05},
                                          std::byte{'H'},
                                          std::byte{'e'},
                                          std::byte{'l'},
                                          std::byte{'l'},
                                          std::byte{'o'}};
    EXPECT_EQ(out, expected);
    EXPECT_FALSE(s.has_pending());
}

TEST(OutboundScheduler, AnEmptyMessageIsOneEmptyFinFrame) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, {}).has_value());
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.frames, (std::vector<WireFrame>{{Opcode::Binary, true, ""}}));
}

TEST(OutboundScheduler, ALargeMessageIsFragmentedAtTheConfiguredFrameSize) {  // EC-F31
    for (const std::size_t size : {63U, 64U, 65U, 127U, 128U, 129U, 200U}) {
        OutboundScheduler s{config(64)};
        const std::string payload(size, 'x');
        ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of(payload)).has_value());
        const auto parsed = parse_server_output(drain(s));
        ASSERT_TRUE(parsed.ok) << size;
        const std::size_t frames = (size + 63) / 64;
        ASSERT_EQ(parsed.frames.size(), frames) << size;
        std::string joined;
        for (std::size_t i = 0; i < frames; ++i) {
            EXPECT_EQ(parsed.frames[i].opcode, i == 0 ? Opcode::Text : Opcode::Continuation)
                << size;
            EXPECT_EQ(parsed.frames[i].fin, i + 1 == frames) << size;
            EXPECT_LE(parsed.frames[i].payload.size(), 64U) << size;
            joined += parsed.frames[i].payload;
        }
        EXPECT_EQ(joined, payload) << size;
    }
}

TEST(OutboundScheduler, MessagesGoOutInTheOrderTheyWereQueued) {
    OutboundScheduler s{config(8)};
    ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of(std::string(20, 'a'))).has_value());
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of("bb")).has_value());
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.frames.size(), 4U);
    EXPECT_EQ(parsed.frames[2], (WireFrame{Opcode::Continuation, true, std::string(4, 'a')}));
    EXPECT_EQ(parsed.frames[3], (WireFrame{Opcode::Binary, true, "bb"}));
}

TEST(OutboundScheduler, ByteAtATimeConsumptionProducesIdenticalBytes) {
    const std::string payload(150, 'q');
    OutboundScheduler whole{config(40)};
    OutboundScheduler slow{config(40)};
    ASSERT_TRUE(whole.enqueue_message(Opcode::Text, bytes_of(payload)).has_value());
    ASSERT_TRUE(slow.enqueue_message(Opcode::Text, bytes_of(payload)).has_value());
    whole.queue_pong(bytes_of("p"));
    slow.queue_pong(bytes_of("p"));
    EXPECT_EQ(drain(whole), drain(slow, 1));
}

TEST(OutboundScheduler, AControlFrameWaitsForTheFrameInFlightThenJumpsTheQueue) {  // EC-F32, C01
    OutboundScheduler s{config(64)};
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(std::string(200, 'd'))).has_value());
    const wsrv::ByteView first = s.pending();  // first fragment: 2 header + 64 payload bytes
    ASSERT_EQ(first.size(), 66U);
    std::vector<std::byte> out(first.begin(), first.begin() + 10);
    s.consume(10);  // the kernel took only 10 bytes: the frame is in flight
    s.queue_pong(bytes_of("pong"));
    const wsrv::ByteView rest = s.pending();
    ASSERT_EQ(rest.size(), 56U);  // still the same frame, nothing inserted inside it
    out.insert(out.end(), rest.begin(), rest.end());
    s.consume(rest.size());
    const std::vector<std::byte> tail = drain(s);
    out.insert(out.end(), tail.begin(), tail.end());
    const auto parsed = parse_server_output(out);
    ASSERT_TRUE(parsed.ok);
    ASSERT_GE(parsed.frames.size(), 3U);
    EXPECT_EQ(parsed.frames[0].opcode, Opcode::Binary);
    EXPECT_FALSE(parsed.frames[0].fin);
    EXPECT_EQ(parsed.frames[1], (WireFrame{Opcode::Pong, true, "pong"}));  // next frame boundary
    EXPECT_EQ(parsed.frames[2].opcode, Opcode::Continuation);
}

TEST(OutboundScheduler, OnlyTheLatestUnsentPongIsKept) {  // EC-C02
    OutboundScheduler s{config()};
    s.queue_pong(bytes_of("one"));
    s.queue_pong(bytes_of("two"));
    s.queue_pong(bytes_of("three"));
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.frames, (std::vector<WireFrame>{{Opcode::Pong, true, "three"}}));
}

TEST(OutboundScheduler, OnePingMayWaitAndAnotherIsBackpressure) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.queue_ping(bytes_of("a")).has_value());
    const auto second = s.queue_ping(bytes_of("b"));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::Backpressure);
    drain(s);
    EXPECT_TRUE(s.queue_ping(bytes_of("c")).has_value());  // the slot is free again
    const auto too_long = s.queue_ping(bytes_of(std::string(126, 'x')));
    ASSERT_FALSE(too_long.has_value());
    EXPECT_EQ(too_long.error().code, ErrorCode::InvalidArgument);
}

TEST(OutboundScheduler, CloseDropsUnstartedDataFinishesTheFrameInFlightAndEndsTheStream) {
    OutboundScheduler s{config(64)};
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(std::string(200, 'd'))).has_value());
    ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("later")).has_value());
    const wsrv::ByteView first = s.pending();
    ASSERT_EQ(first.size(), 66U);
    std::vector<std::byte> wire(first.begin(), first.begin() + 5);
    s.consume(5);  // the first fragment is in flight
    s.queue_close(1000, "bye");
    EXPECT_TRUE(s.close_queued());
    EXPECT_FALSE(s.close_flushed());
    const std::vector<std::byte> rest = drain(s);
    wire.insert(wire.end(), rest.begin(), rest.end());
    const auto parsed = parse_server_output(wire);
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.frames.size(), 2U);  // the unfinished fragment, then Close, nothing else
    EXPECT_EQ(parsed.frames[0].opcode, Opcode::Binary);
    EXPECT_FALSE(parsed.frames[0].fin);
    EXPECT_EQ(parsed.frames[1],
              (WireFrame{Opcode::Close, true, wsrv::test::close_payload(1000, "bye")}));
    EXPECT_TRUE(s.close_flushed());
    EXPECT_FALSE(s.has_pending());
    const auto after = s.enqueue_message(Opcode::Text, bytes_of("x"));
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, ErrorCode::Closing);
}

TEST(OutboundScheduler, CloseIsTheOnlyThingSentWhenNothingWasInFlight) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("unsent")).has_value());
    s.queue_pong(bytes_of("p"));
    s.queue_close(1001, "going");
    s.queue_close(1000, "second close is ignored");
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.frames.size(), 1U);
    EXPECT_EQ(parsed.frames[0].opcode, Opcode::Close);
    EXPECT_EQ(parsed.frames[0].payload, wsrv::test::close_payload(1001, "going"));
}

TEST(OutboundScheduler, ACloseWithoutStatusIsAnEmptyCloseFrame) {  // EC-C05
    OutboundScheduler s{config()};
    s.queue_close(std::nullopt, "");
    const auto parsed = parse_server_output(drain(s));
    EXPECT_EQ(parsed.frames, (std::vector<WireFrame>{{Opcode::Close, true, ""}}));
}

TEST(OutboundScheduler, TheQueueBoundIsExactAndNothingIsQueuedOnError) {  // EC-N09
    OutboundScheduler s{config(64, 100)};
    // 16 bytes of overhead are charged per message: 84 + 16 == 100 fits, one more byte does not.
    EXPECT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(std::string(84, 'a'))).has_value());
    const auto full = s.enqueue_message(Opcode::Binary, {});
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error().code, ErrorCode::Backpressure);
    EXPECT_EQ(parse_server_output(drain(s)).frames.size(), 2U);  // 84 bytes at 64 per frame
    // Draining frees the space.
    EXPECT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(std::string(84, 'a'))).has_value());
    // A message that can never fit is a different error from "try later".
    OutboundScheduler small{config(64, 100)};
    const auto never = small.enqueue_message(Opcode::Binary, bytes_of(std::string(85, 'a')));
    ASSERT_FALSE(never.has_value());
    EXPECT_EQ(never.error().code, ErrorCode::BufferFull);
    EXPECT_TRUE(drain(small).empty());
}

TEST(OutboundScheduler, ManyEmptyMessagesCannotGrowTheQueueWithoutBound) {
    OutboundScheduler s{config(64, 160)};
    int accepted = 0;
    while (s.enqueue_message(Opcode::Text, {}).has_value()) {
        ++accepted;
        ASSERT_LT(accepted, 1000);
    }
    EXPECT_EQ(accepted, 10);  // 16 bytes each
}

TEST(OutboundScheduler, TheWireBufferStaysSmallWhateverTheMessageSize) {
    OutboundScheduler s{config(1024, 8U << 20)};
    ASSERT_TRUE(
        s.enqueue_message(Opcode::Binary, bytes_of(std::string(4U << 20, 'z'))).has_value());
    std::size_t largest = 0;
    for (;;) {
        const wsrv::ByteView view = s.pending();
        if (view.empty()) {
            break;
        }
        largest = std::max(largest, view.size());
        s.consume(view.size());
    }
    EXPECT_LE(largest, 1024U + wsrv::kMaxHeaderSize);
}

}  // namespace
