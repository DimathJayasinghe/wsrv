// SPDX-License-Identifier: Apache-2.0
#include "protocol/outbound_scheduler.hpp"

#include "support/sanitized.hpp"
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

// ADR-0043: largest_message() is the exact boundary of can_enqueue() on an empty queue, so a caller
// that refuses a larger message early refuses exactly what the queue would have dropped.
TEST(OutboundScheduler, LargestMessageIsTheExactSizeAnEmptyQueueStillTakes) {
    const OutboundConfig c = config(64, 1000);
    const std::size_t largest = wsrv::largest_message(c);
    EXPECT_EQ(largest, 1000U - wsrv::kMessageOverhead);
    const OutboundScheduler s{c};
    EXPECT_TRUE(s.can_enqueue(largest).has_value());
    const auto over = s.can_enqueue(largest + 1);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code, ErrorCode::BufferFull) << "refused however empty the queue is";
}

TEST(OutboundScheduler, ATinyQueueHoldsNoMessageAtAll) {
    EXPECT_EQ(wsrv::largest_message(config(64, wsrv::kMessageOverhead)), 0U);
    EXPECT_EQ(wsrv::largest_message(config(64, 3)), 0U);
}

TEST(OutboundScheduler, ACompressedMessageCarriesRsv1OnItsFirstFrameOnly) {  // EC-D02
    OutboundScheduler s{config(4)};
    const std::string payload(10, 'x');
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(payload), true).has_value());
    ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("plain")).has_value());
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.frames.size(), 5U);  // 4 + 4 + 2 bytes, then "plain" in 4 + 1
    EXPECT_TRUE(parsed.frames[0].rsv1);
    EXPECT_FALSE(parsed.frames[1].rsv1);
    EXPECT_FALSE(parsed.frames[2].rsv1);
    EXPECT_FALSE(parsed.frames[3].rsv1);  // the next message is not compressed
    EXPECT_FALSE(parsed.frames[4].rsv1);
}

TEST(OutboundScheduler, CanEnqueueAnswersWhatEnqueueWouldAnswerWithoutQueueingAnything) {
    OutboundScheduler s{config(64, 100)};
    EXPECT_TRUE(s.can_enqueue(50).has_value());
    EXPECT_EQ(s.can_enqueue(100).error().code, ErrorCode::BufferFull);  // can never fit
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(std::string(60, 'a'))).has_value());
    EXPECT_EQ(s.can_enqueue(50).error().code, ErrorCode::Backpressure);  // fits only when empty
    EXPECT_TRUE(s.can_enqueue(8).has_value());
    ASSERT_TRUE(s.begin_stream(Opcode::Binary).has_value());
    EXPECT_EQ(s.can_enqueue(1).error().code, ErrorCode::MessageInProgress);
    s.queue_close(std::nullopt, "");
    EXPECT_EQ(s.can_enqueue(1).error().code, ErrorCode::Closing);
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

TEST(OutboundScheduler, EveryRefillIsExactlyOneFrameSoAControlFrameNeverWaitsForAnUnstartedOne) {
    // EC-F32: only a frame already partially written is finished first. A Pong is half out when
    // the Close is queued: the Close follows the Pong, not the 1000-byte data frame behind it.
    OutboundScheduler s{config(1000)};
    s.queue_pong(bytes_of("pong"));
    ASSERT_TRUE(s.enqueue_message(Opcode::Binary, bytes_of(std::string(5000, 'd'))).has_value());
    ASSERT_EQ(s.pending().size(), 6U);  // just the Pong frame: 2 + 4
    s.consume(1);
    s.queue_close(1000, "");
    std::vector<std::byte> rest = drain(s);
    EXPECT_EQ(rest.size(), 5U + 4U);  // the other 5 Pong bytes, then the 4-byte Close frame
    EXPECT_EQ(static_cast<unsigned>(rest[5]) & 0x0FU, 0x8U);  // Close opcode right after the Pong
}

TEST(OutboundScheduler, EveryUnsentPongIsSentInOrderUpToTheBound) {  // EC-C02, Autobahn 2.10
    OutboundScheduler s{config()};
    std::vector<WireFrame> expected;
    for (std::size_t i = 0; i < wsrv::kMaxQueuedPongs; ++i) {
        const std::string payload = std::to_string(i);
        s.queue_pong(bytes_of(payload));
        expected.push_back({Opcode::Pong, true, payload});
    }
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.frames, expected);
}

TEST(OutboundScheduler, BeyondTheBoundTheNewestPongReplacesTheLastOneQueued) {  // EC-C02
    OutboundScheduler s{config()};
    for (std::size_t i = 0; i < wsrv::kMaxQueuedPongs + 4; ++i) {
        s.queue_pong(bytes_of(std::to_string(i)));
    }
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.frames.size(), wsrv::kMaxQueuedPongs);  // memory stays bounded
    EXPECT_EQ(parsed.frames.front().payload, "0");           // the oldest still go out first
    EXPECT_EQ(parsed.frames.back().payload,
              std::to_string(wsrv::kMaxQueuedPongs + 3));  // the latest wins
}

TEST(OutboundScheduler, AQueuedCloseDropsEveryUnsentPong) {
    OutboundScheduler s{config()};
    for (int i = 0; i < 5; ++i) {
        s.queue_pong(bytes_of("p"));
    }
    s.queue_close(1000, "");
    const auto parsed = parse_server_output(drain(s));
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ(parsed.frames.size(), 1U);
    EXPECT_EQ(parsed.frames[0].opcode, Opcode::Close);
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

TEST(OutboundScheduler, AnAllocationFailureWhileQueueingIsAnErrorAndLeavesTheQueueIntact) {
#ifdef WSRV_TEST_SANITIZED
    GTEST_SKIP() << "sanitizer allocators abort instead of throwing bad_alloc; covered by release";
#endif
    OutboundScheduler s{config(64, std::size_t{1} << 60)};
    const std::byte anchor{0};
    const wsrv::ByteView absurd{&anchor,
                                std::size_t{1} << 50};  // never read: allocation fails first
    const auto r = s.enqueue_message(Opcode::Binary, absurd);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::ResourceExhausted);
    EXPECT_FALSE(s.has_pending());
    EXPECT_FALSE(s.failed());
    EXPECT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("still works")).has_value());
    EXPECT_EQ(parse_server_output(drain(s)).frames.size(), 1U);
}

// ---- streaming (ADR-0015): a message whose bytes arrive piece by piece
// ----------------------------

/// The frames the scheduler writes right now, parsed.
std::vector<WireFrame> frames_now(OutboundScheduler& s) {
    const auto parsed = parse_server_output(drain(s));
    EXPECT_TRUE(parsed.ok);
    return parsed.frames;
}

TEST(OutboundStream, PiecesBecomeFragmentsAndAnEmptyFinalContinuationEndsTheMessage) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.begin_stream(Opcode::Text).has_value());
    ASSERT_TRUE(s.append_stream(bytes_of("abc")).has_value());
    auto first = frames_now(s);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first[0].opcode, Opcode::Text);
    EXPECT_FALSE(first[0].fin);
    EXPECT_EQ(first[0].payload, "abc");

    ASSERT_TRUE(s.append_stream(bytes_of("de")).has_value());
    auto second = frames_now(s);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].opcode, Opcode::Continuation);
    EXPECT_FALSE(second[0].fin);
    EXPECT_EQ(second[0].payload, "de");

    ASSERT_TRUE(s.end_stream().has_value());
    auto last = frames_now(s);
    ASSERT_EQ(last.size(), 1U);
    EXPECT_EQ(last[0].opcode, Opcode::Continuation);  // EC-F37: an empty FIN frame is valid
    EXPECT_TRUE(last[0].fin);
    EXPECT_TRUE(last[0].payload.empty());
    EXPECT_FALSE(s.has_pending());
    EXPECT_FALSE(s.stream_open());
}

TEST(OutboundStream, NothingIsWrittenWhileAnOpenMessageHasNoBytes) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.begin_stream(Opcode::Binary).has_value());
    EXPECT_FALSE(s.has_pending());
    EXPECT_TRUE(s.pending().empty());
    EXPECT_TRUE(s.stream_open());
}

TEST(OutboundStream, AMessageThatEndsWithoutBytesIsOneEmptyFinFrame) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.begin_stream(Opcode::Text).has_value());
    ASSERT_TRUE(s.end_stream().has_value());
    const auto frames = frames_now(s);
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].opcode, Opcode::Text);
    EXPECT_TRUE(frames[0].fin);
}

TEST(OutboundStream, ALargePieceIsSplitAtTheFrameLimit) {
    OutboundScheduler s{config(64)};
    ASSERT_TRUE(s.begin_stream(Opcode::Binary).has_value());
    ASSERT_TRUE(s.append_stream(bytes_of(std::string(150, 'x'))).has_value());
    const auto frames = frames_now(s);
    ASSERT_EQ(frames.size(), 3U);  // 64 + 64 + 22, none final: the message is still open
    EXPECT_EQ(frames[0].opcode, Opcode::Binary);
    EXPECT_EQ(frames[1].opcode, Opcode::Continuation);
    EXPECT_EQ(frames[2].payload.size(), 22U);
    for (const WireFrame& frame : frames) {
        EXPECT_FALSE(frame.fin);
    }
}

TEST(OutboundStream, ASecondMessageWhileOneIsOpenIsRefusedAndWritesNothing) {  // EC-F33
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.begin_stream(Opcode::Text).has_value());
    for (const auto& result :
         {s.enqueue_message(Opcode::Text, bytes_of("x")), s.begin_stream(Opcode::Binary)}) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::MessageInProgress);
    }
    EXPECT_FALSE(s.has_pending());
    ASSERT_TRUE(s.end_stream().has_value());
    EXPECT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("after")).has_value());  // allowed again
}

TEST(OutboundStream, TheQueueBoundAppliesToStreamedBytes) {  // EC-F35
    OutboundScheduler s{config(64, 100)};
    ASSERT_TRUE(s.begin_stream(Opcode::Binary).has_value());  // costs the message overhead
    ASSERT_TRUE(s.append_stream(bytes_of(std::string(60, 'a'))).has_value());
    const auto refused = s.append_stream(bytes_of(std::string(60, 'b')));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::Backpressure);
    static_cast<void>(drain(s));  // the first 60 bytes leave...
    EXPECT_TRUE(s.append_stream(bytes_of(std::string(60, 'b'))).has_value());  // ...room again
}

TEST(OutboundStream, ControlFramesGoAheadOfAStreamedMessage) {  // FR-F10
    OutboundScheduler s{config(8)};
    ASSERT_TRUE(s.begin_stream(Opcode::Binary).has_value());
    ASSERT_TRUE(s.append_stream(bytes_of(std::string(20, 'x'))).has_value());
    s.queue_pong(bytes_of("p"));
    const auto frames = frames_now(s);
    ASSERT_GE(frames.size(), 2U);
    EXPECT_EQ(frames[0].opcode, Opcode::Pong);
    EXPECT_EQ(frames[1].opcode, Opcode::Binary);
}

TEST(OutboundStream, AStreamQueuedBehindWholeMessagesKeepsTheOrder) {
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.enqueue_message(Opcode::Text, bytes_of("one")).has_value());
    ASSERT_TRUE(s.begin_stream(Opcode::Text).has_value());
    ASSERT_TRUE(s.append_stream(bytes_of("two")).has_value());
    ASSERT_TRUE(s.end_stream().has_value());
    const auto frames = frames_now(s);
    ASSERT_EQ(frames.size(), 2U);  // ended before it was framed: one single FIN frame, no tail
    EXPECT_EQ(frames[0].payload, "one");
    EXPECT_TRUE(frames[0].fin);
    EXPECT_EQ(frames[1].opcode, Opcode::Text);
    EXPECT_EQ(frames[1].payload, "two");
    EXPECT_TRUE(frames[1].fin);
}

TEST(OutboundStream, CloseDiscardsAnOpenMessageAndLaterCallsReportClosing) {  // EC-C14
    OutboundScheduler s{config()};
    ASSERT_TRUE(s.begin_stream(Opcode::Text).has_value());
    ASSERT_TRUE(s.append_stream(bytes_of("unsent")).has_value());
    s.queue_close(1000, "bye");
    EXPECT_FALSE(s.stream_open());
    EXPECT_EQ(s.append_stream(bytes_of("x")).error().code, ErrorCode::Closing);
    EXPECT_EQ(s.end_stream().error().code, ErrorCode::Closing);
    const auto frames = frames_now(s);
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].opcode, Opcode::Close);
}

TEST(OutboundStream, AppendOrEndWithoutAnOpenMessageIsAnArgumentError) {
    OutboundScheduler s{config()};
    EXPECT_EQ(s.append_stream(bytes_of("x")).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(s.end_stream().error().code, ErrorCode::InvalidArgument);
}

}  // namespace
