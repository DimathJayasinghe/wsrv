// SPDX-License-Identifier: Apache-2.0
// SessionHandle and LoopLink: sending to a connection from another thread (ADR-0014 section 4,
// EC-N13, EC-N14). Uses a real EventLoop pumped by the test thread and a fake directory of
// sessions, so every outcome (delivered, dropped, refused) is deterministic.
#include "server/session_handle.hpp"

#include "base/clock.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "support/recording_session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::CloseCode;
using wsrv::ConnectionId;
using wsrv::ErrorCode;
using wsrv::SessionHandle;
using wsrv::test::FakeDirectory;
using wsrv::test::RecordingSession;

class SessionHandleTest : public ::testing::Test {
protected:
    explicit SessionHandleTest(std::size_t max_tasks = 4096)
        : loop_{make_loop(max_tasks)}, link_{std::make_shared<wsrv::LoopLink>(*loop_, directory_)} {
        directory_.sessions[kId] = &session_;
    }

    static std::unique_ptr<wsrv::EventLoop> make_loop(std::size_t max_tasks) {
        wsrv::EventLoopConfig config;
        config.max_queued_tasks = max_tasks;
        auto loop = wsrv::EventLoop::create(sys_, clock_, config);
        EXPECT_TRUE(loop.has_value());
        return std::move(*loop);
    }

    /// Run the loop on this thread until `done()` or a timeout.
    template <class Done>
    bool pump_until(Done&& done) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            EXPECT_TRUE(loop_->run_once(10ms).has_value());
        }
        return true;
    }

    static constexpr ConnectionId kId = 7;
    static inline wsrv::LinuxSyscalls sys_;
    static inline wsrv::SteadyClock clock_;
    FakeDirectory directory_;
    RecordingSession session_{kId};
    std::unique_ptr<wsrv::EventLoop> loop_;
    std::shared_ptr<wsrv::LoopLink> link_;
};

class SmallQueueTest : public SessionHandleTest {
protected:
    SmallQueueTest() : SessionHandleTest{4} {}
};

TEST_F(SessionHandleTest, AMessageSentFromAnotherThreadRunsOnTheLoopThreadInOrder) {  // FR-S3
    const SessionHandle handle{link_, kId};
    std::thread producer([&] {
        for (int i = 0; i < 500; ++i) {
            while (!handle.send_text(std::to_string(i)).has_value()) {
                std::this_thread::yield();  // the loop is behind: the queue is bounded
            }
        }
    });
    const bool done = pump_until([&] { return session_.texts.size() == 500; });
    producer.join();
    ASSERT_TRUE(done);
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(session_.texts[static_cast<std::size_t>(i)], std::to_string(i));
    }
    for (const std::thread::id& thread : session_.threads) {
        EXPECT_EQ(thread, std::this_thread::get_id());
    }
}

// ---- SharedMessage (ADR-0024 section 3) ---------------------------------------------------------

TEST(SharedMessage, TextIsValidatedOnceWhenItIsBuilt) {
    EXPECT_TRUE(wsrv::SharedMessage::text("h\xC3\xA9llo").has_value());
    const auto bad = wsrv::SharedMessage::text("\xC3\x28");  // an invalid two-byte sequence
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidUtf8);
    EXPECT_FALSE(wsrv::SharedMessage::text("\xE2\x82").has_value()) << "cut in the middle";
}

TEST(SharedMessage, ACopyIsTheSamePayloadNotANewAllocation) {
    const auto message = wsrv::SharedMessage::binary(wsrv::ByteView{});
    ASSERT_TRUE(message.has_value());
    const wsrv::SharedMessage copy = *message;
    EXPECT_EQ(copy.data().data(), message->data().data());
    EXPECT_FALSE(copy.is_text());
}

TEST_F(SessionHandleTest, ASharedMessageReachesTheSessionAsTextOrBinary) {
    const SessionHandle handle{link_, kId};
    const auto text = wsrv::SharedMessage::text("shared hello");
    const std::string raw = "\x01\x02\x03";
    const auto binary = wsrv::SharedMessage::binary(wsrv::as_bytes(raw));
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(binary.has_value());
    ASSERT_TRUE(handle.send(*text).has_value());
    ASSERT_TRUE(handle.send(*binary).has_value());
    ASSERT_TRUE(pump_until([&] { return !session_.texts.empty() && !session_.binaries.empty(); }));
    EXPECT_EQ(session_.texts[0], "shared hello");
    EXPECT_EQ(session_.binaries[0], raw);
}

TEST_F(SessionHandleTest, ManyHandlesCanSendTheSameSharedMessage) {
    const auto message = wsrv::SharedMessage::text("same for all");
    ASSERT_TRUE(message.has_value());
    const SessionHandle handle{link_, kId};
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(handle.send(*message).has_value());
    }
    ASSERT_TRUE(pump_until([&] { return session_.texts.size() == 50; }));
    EXPECT_EQ(session_.texts.front(), session_.texts.back());
}

TEST_F(SessionHandleTest, ASharedMessageToAClosedLinkIsClosingAndAnInertHandleSaysSo) {
    const auto message = wsrv::SharedMessage::text("x");
    ASSERT_TRUE(message.has_value());
    const SessionHandle handle{link_, kId};
    link_->close();
    const auto sent = handle.send(*message);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code, ErrorCode::Closing);
    const SessionHandle inert;
    EXPECT_EQ(inert.send(*message).error().code, ErrorCode::Closing);
}

TEST_F(SessionHandleTest, BinaryAndCloseReachTheSession) {
    const SessionHandle handle{link_, kId};
    const std::vector<std::byte> bytes(10, std::byte{1});
    ASSERT_TRUE(handle.send_binary(bytes).has_value());
    ASSERT_TRUE(handle.close(CloseCode::GoingAway, "bye").has_value());
    ASSERT_TRUE(pump_until([&] { return !session_.closes.empty(); }));
    EXPECT_EQ(session_.binary_sizes, std::vector<std::size_t>{10});
    EXPECT_EQ(session_.closes.front().first, CloseCode::GoingAway);
    EXPECT_EQ(session_.closes.front().second, "bye");
}

TEST_F(SessionHandleTest, ACloseTheWireCannotCarryIsRefusedToTheCallerAndNothingIsQueued) {
    const SessionHandle handle{link_, kId};
    for (const std::uint16_t code : {std::uint16_t{0},
                                     std::uint16_t{999},
                                     std::uint16_t{1004},
                                     std::uint16_t{1005},
                                     std::uint16_t{1006},
                                     std::uint16_t{1015},
                                     std::uint16_t{2999},
                                     std::uint16_t{5000}}) {
        const wsrv::Result<void> sent = handle.close(static_cast<CloseCode>(code), "x");
        ASSERT_FALSE(sent.has_value()) << code;
        EXPECT_EQ(sent.error().code, ErrorCode::InvalidArgument);
    }
    const wsrv::Result<void> bad_reason = handle.close(CloseCode::Normal, "bad\xFF");
    ASSERT_FALSE(bad_reason.has_value());
    EXPECT_EQ(bad_reason.error().code, ErrorCode::InvalidUtf8);

    ASSERT_TRUE(handle.close(static_cast<CloseCode>(4000), "caf\xC3\xA9").has_value());
    ASSERT_TRUE(pump_until([&] { return !session_.closes.empty(); }));
    EXPECT_EQ(session_.closes.size(), 1U);  // only the valid one
}

TEST_F(SessionHandleTest, AResumeReachesTheSessionOnTheLoopThread) {
    const SessionHandle handle{link_, kId};
    ASSERT_TRUE(handle.resume_reads().has_value());
    ASSERT_TRUE(pump_until([&] { return session_.resumes == 1; }));
}

TEST_F(SessionHandleTest, TextThatIsNotUtf8IsRefusedToTheCallerAndNothingIsQueued) {  // FR-F7
    const SessionHandle handle{link_, kId};
    for (const std::string& bad : {std::string{"ok\xFF"}, std::string{"cut\xE2\x82"}}) {
        const wsrv::Result<void> sent = handle.send_text(bad);
        ASSERT_FALSE(sent.has_value());
        EXPECT_EQ(sent.error().code, ErrorCode::InvalidUtf8);
    }
    ASSERT_TRUE(handle.send_text("caf\xC3\xA9").has_value());
    ASSERT_TRUE(pump_until([&] { return !session_.texts.empty(); }));
    EXPECT_EQ(session_.texts, std::vector<std::string>{"caf\xC3\xA9"});  // only the valid one
}

TEST_F(SessionHandleTest, ACommandForASessionThatIsGoneIsDroppedWithoutHarm) {  // EC-N13
    const SessionHandle handle{link_, kId};
    ASSERT_TRUE(handle.send_text("late").has_value());  // accepted for delivery...
    directory_.sessions.clear();                        // ...but the connection ended meanwhile
    ASSERT_TRUE(handle.send_text("later").has_value());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(loop_->run_once(10ms).has_value());
    }
    EXPECT_TRUE(session_.texts.empty());
}

TEST_F(SessionHandleTest, AClosedLinkRefusesWithClosing) {  // the shard is gone: EC-N13
    const SessionHandle handle{link_, kId};
    link_->close();
    const auto result = handle.send_text("x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Closing);
    EXPECT_EQ(handle.close(CloseCode::Normal, "").error().code, ErrorCode::Closing);
}

TEST_F(SessionHandleTest, ADefaultConstructedHandleRefusesWithClosing) {
    const SessionHandle inert;
    EXPECT_EQ(inert.send_text("x").error().code, ErrorCode::Closing);
    EXPECT_EQ(inert.id(), 0U);
}

TEST_F(SessionHandleTest, AHandleOutlivesTheLinkOwnerAndCopiesShareTheTarget) {
    SessionHandle copy;
    {
        const SessionHandle original{link_, kId};
        copy = original;
    }
    link_.reset();  // the copy keeps the link alive: no dangling, the loop is still here
    ASSERT_TRUE(copy.send_text("still works").has_value());
    ASSERT_TRUE(pump_until([&] { return !session_.texts.empty(); }));
    EXPECT_EQ(copy.id(), kId);
}

TEST_F(SmallQueueTest, AFullTaskQueueIsReportedAsBufferFull) {  // EC-N14
    const SessionHandle handle{link_, kId};
    int accepted = 0;
    wsrv::Result<void> last;
    for (int i = 0; i < 10; ++i) {
        last = handle.send_text("m");
        accepted += last.has_value() ? 1 : 0;
    }
    EXPECT_EQ(accepted, 4);
    ASSERT_FALSE(last.has_value());
    EXPECT_EQ(last.error().code, ErrorCode::BufferFull);
    ASSERT_TRUE(pump_until([&] { return session_.texts.size() == 4; }));
}

TEST_F(SmallQueueTest,
       ABroadcastStopsBeforeTheQueueIsFullSoAControlTaskStillGetsIn) {  // ADR-0024 A1
    const SessionHandle handle{link_, kId};
    const auto message = wsrv::SharedMessage::text("m");
    ASSERT_TRUE(message.has_value());
    int accepted = 0;
    wsrv::Result<void> last;
    for (int i = 0; i < 10; ++i) {
        last = handle.send(*message);
        accepted += last.has_value() ? 1 : 0;
    }
    EXPECT_EQ(accepted, 3) << "a queue of 4 keeps one slot back for control";
    ASSERT_FALSE(last.has_value());
    EXPECT_EQ(last.error().code, ErrorCode::BufferFull);
    EXPECT_TRUE(handle.close(CloseCode::GoingAway, "bye").has_value())
        << "the broadcast flood must not keep a close out";
    ASSERT_TRUE(pump_until([&] { return !session_.closes.empty(); }));
    EXPECT_EQ(session_.texts.size(), 3U);
}

TEST_F(SessionHandleTest, ClosingTheLinkWhileThreadsSendIsRaceFree) {  // run under TSan
    const SessionHandle handle{link_, kId};
    std::atomic<bool> stop{false};
    std::vector<std::thread> senders;
    for (int t = 0; t < 4; ++t) {
        senders.emplace_back([&] {
            while (!stop.load()) {
                const auto result = handle.send_text("x");
                if (!result.has_value() && result.error().code == ErrorCode::Closing) {
                    return;
                }
                std::this_thread::yield();
            }
        });
    }
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(loop_->run_once(1ms).has_value());
    }
    link_->close();  // the shard shuts down while senders are mid-call
    stop.store(true);
    for (std::thread& sender : senders) {
        sender.join();
    }
    SUCCEED();
}

}  // namespace
