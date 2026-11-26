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
#include <memory>
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

// ---- the pending-send byte budget (ADR-0032) ----------------------------------------------------

class BudgetTest : public ::testing::Test {
protected:
    static constexpr ConnectionId kId = 7;
    static constexpr std::size_t kBudget = 100;

    BudgetTest() {
        wsrv::EventLoopConfig config;
        auto loop = wsrv::EventLoop::create(sys_, clock_, config);
        EXPECT_TRUE(loop.has_value());
        loop_ = std::move(*loop);
        link_ = std::make_shared<wsrv::LoopLink>(*loop_, directory_, kBudget);
        directory_.sessions[kId] = &session_;
    }

    void run_loop() {
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(loop_->run_once(10ms).has_value());
        }
    }

    static inline wsrv::LinuxSyscalls sys_;
    static inline wsrv::SteadyClock clock_;
    FakeDirectory directory_;
    RecordingSession session_{kId};
    std::unique_ptr<wsrv::EventLoop> loop_;
    std::shared_ptr<wsrv::LoopLink> link_;
};

TEST_F(BudgetTest, DirectSendsAreRefusedOnceTheBytesWaitingForTheLoopReachTheBudget) {
    const SessionHandle handle{link_, kId};
    EXPECT_TRUE(handle.send_text(std::string(60, 'a')).has_value());
    EXPECT_EQ(link_->pending_send_bytes(), 60U);
    const auto over = handle.send_text(std::string(60, 'b'));
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code, ErrorCode::BufferFull) << "wait for the loop and try again";
    EXPECT_EQ(link_->pending_send_bytes(), 60U) << "a refused send holds nothing";
    EXPECT_TRUE(handle.send_text(std::string(40, 'c')).has_value()) << "exactly to the budget";
    EXPECT_EQ(link_->pending_send_bytes(), 100U);
    EXPECT_EQ(handle.send_text("x").error().code, ErrorCode::BufferFull);
}

TEST_F(BudgetTest, TheBytesAreGivenBackWhenTheLoopHasRunTheSend) {
    const SessionHandle handle{link_, kId};
    ASSERT_TRUE(handle.send_text(std::string(90, 'a')).has_value());
    ASSERT_FALSE(handle.send_binary(std::vector<std::byte>(90)).has_value());
    run_loop();
    EXPECT_EQ(link_->pending_send_bytes(), 0U);
    EXPECT_EQ(session_.texts.size(), 1U);
    ASSERT_TRUE(handle.send_binary(std::vector<std::byte>(90)).has_value()) << "room again";
}

TEST_F(BudgetTest, ASendLargerThanTheWholeBudgetCanNeverBeSentAndIsInvalidNotBusy) {
    const SessionHandle handle{link_, kId};
    const auto too_big = handle.send_text(std::string(kBudget + 1, 'a'));
    ASSERT_FALSE(too_big.has_value());
    EXPECT_EQ(too_big.error().code, ErrorCode::InvalidArgument) << "retrying cannot help";
    EXPECT_EQ(handle.send_binary(std::vector<std::byte>(kBudget + 1)).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(link_->pending_send_bytes(), 0U);
    EXPECT_TRUE(handle.send_text(std::string(kBudget, 'a')).has_value())
        << "exactly the budget fits";
}

TEST_F(BudgetTest, SendsThatTheLoopNeverRunsAreGivenBackWhenTheLoopIsDestroyed) {
    const SessionHandle handle{link_, kId};
    ASSERT_TRUE(handle.send_text(std::string(80, 'a')).has_value());
    ASSERT_EQ(link_->pending_send_bytes(), 80U);
    link_->close();
    loop_.reset();  // the queued task is discarded, never run
    EXPECT_EQ(link_->pending_send_bytes(), 0U);
}

TEST_F(BudgetTest, ASendTheFullTaskQueueRefusesHoldsNoBudget) {
    wsrv::EventLoopConfig config;
    config.max_queued_tasks = 1;
    auto small = wsrv::EventLoop::create(sys_, clock_, config);
    ASSERT_TRUE(small.has_value());
    auto link = std::make_shared<wsrv::LoopLink>(**small, directory_, kBudget);
    const SessionHandle handle{link, kId};
    ASSERT_TRUE(handle.send_text("first").has_value());
    const auto refused = handle.send_text("second");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(link->pending_send_bytes(), 5U) << "only the one that was queued";
}

TEST_F(BudgetTest, BroadcastsControlCommandsAndDecisionsAreNotChargedToTheBudget) {
    const SessionHandle handle{link_, kId};
    const auto big = wsrv::SharedMessage::text(std::string(10 * kBudget, 'a'));
    ASSERT_TRUE(big.has_value());
    EXPECT_TRUE(handle.send(*big).has_value()) << "one shared buffer for all subscribers";
    EXPECT_TRUE(handle.close(CloseCode::Normal, "bye").has_value());
    EXPECT_TRUE(handle.resume_reads().has_value());
    EXPECT_EQ(link_->pending_send_bytes(), 0U);
}

TEST_F(BudgetTest, TheBudgetIsPerLink) {
    wsrv::LoopLink other{*loop_, directory_, kBudget};
    const SessionHandle first{link_, kId};
    ASSERT_TRUE(first.send_text(std::string(kBudget, 'a')).has_value());
    EXPECT_EQ(other.pending_send_bytes(), 0U);
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

// ---- one slot per chunk of recipients, not per recipient (ADR-0034) ----------------------------

/// Many connections on one loop with a tiny task queue (4 slots: a bulk lane of 3), so that the
/// per-recipient cost the old broadcast paid is visible: it could take 3 recipients, not 50.
class BulkTest : public SmallQueueTest {
protected:
    /// `count` sessions with ids 100.., each reachable through `link_`.
    std::vector<SessionHandle> populate(std::size_t count) {
        std::vector<SessionHandle> handles;
        for (std::size_t i = 0; i < count; ++i) {
            const ConnectionId id = 100 + i;
            sessions_.push_back(std::make_unique<RecordingSession>(id));
            directory_.sessions[id] = sessions_.back().get();
            handles.emplace_back(link_, id);
        }
        return handles;
    }

    [[nodiscard]] std::size_t delivered() const {
        std::size_t total = 0;
        for (const auto& session : sessions_) {
            total += session->texts.size();
        }
        return total;
    }

    std::vector<std::unique_ptr<RecordingSession>> sessions_;
};

TEST_F(BulkTest, ABroadcastToMoreRecipientsThanTheBulkLaneHasSlotsIsDeliveredToAll) {
    const std::vector<SessionHandle> handles = populate(50);
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    const wsrv::BulkSendReport report = wsrv::send_to_many(handles, *message);
    EXPECT_EQ(report.accepted, 50U) << "one chunk, one slot: the lane of 3 is not the limit";
    EXPECT_EQ(report.refused, 0U);
    EXPECT_TRUE(report.gone.empty());
    ASSERT_TRUE(pump_until([&] { return delivered() == 50; }));
}

TEST_F(BulkTest, AChunkIsAcceptedOrRefusedWholeAndTheCountsAreForRecipients) {
    const std::size_t total = (wsrv::kBulkChunk * 4) + 100;  // five chunks; the lane has three
    const std::vector<SessionHandle> handles = populate(total);
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    const wsrv::BulkSendReport report = wsrv::send_to_many(handles, *message);
    EXPECT_EQ(report.accepted, wsrv::kBulkChunk * 3) << "three chunks fit the lane";
    EXPECT_EQ(report.refused, total - (wsrv::kBulkChunk * 3));
    EXPECT_TRUE(report.gone.empty()) << "a full queue is not a gone server";
    ASSERT_TRUE(pump_until([&] { return delivered() == wsrv::kBulkChunk * 3; }));
}

TEST_F(BulkTest, RecipientsAreServedInTheOrderGiven) {
    const std::vector<SessionHandle> handles = populate(10);
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    // The first publish and the second reach each connection in that order.
    const auto second = wsrv::SharedMessage::text("again");
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(wsrv::send_to_many(handles, *message).accepted, 10U);
    ASSERT_EQ(wsrv::send_to_many(handles, *second).accepted, 10U);
    ASSERT_TRUE(pump_until([&] { return delivered() == 20; }));
    for (const auto& session : sessions_) {
        ASSERT_EQ(session->texts.size(), 2U);
        EXPECT_EQ(session->texts[0], "hi");
        EXPECT_EQ(session->texts[1], "again");
    }
}

TEST_F(BulkTest, AFullBulkLaneRefusesTheWholeChunkAndNothingIsDelivered) {
    const std::vector<SessionHandle> handles = populate(40);
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    const SessionHandle filler{link_, kId};
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(filler.send(*message).has_value());  // the lane (3) is now used up
    }
    const wsrv::BulkSendReport report = wsrv::send_to_many(handles, *message);
    EXPECT_EQ(report.accepted, 0U);
    EXPECT_EQ(report.refused, 40U);
    EXPECT_TRUE(report.gone.empty());
}

TEST_F(BulkTest, AClosedLinkMakesEveryRecipientRefusedAndGone) {
    const std::vector<SessionHandle> handles = populate(5);
    link_->close();
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    const wsrv::BulkSendReport report = wsrv::send_to_many(handles, *message);
    EXPECT_EQ(report.accepted, 0U);
    EXPECT_EQ(report.refused, 5U);
    ASSERT_EQ(report.gone.size(), 5U);
    EXPECT_EQ(report.gone.front(), 100U);
}

TEST_F(BulkTest, AnInertHandleIsRefusedAndGoneAndTheRestStillGetTheMessage) {
    std::vector<SessionHandle> handles = populate(3);
    handles.insert(handles.begin() + 1, SessionHandle{});
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    const wsrv::BulkSendReport report = wsrv::send_to_many(handles, *message);
    EXPECT_EQ(report.accepted, 3U);
    EXPECT_EQ(report.refused, 1U);
    ASSERT_EQ(report.gone.size(), 1U);
    ASSERT_TRUE(pump_until([&] { return delivered() == 3; }));
}

TEST_F(BulkTest, MoreIdsThanOneChunkInOneTaskIsAnInvalidArgument) {
    const std::vector<ConnectionId> ids(wsrv::kBulkChunk + 1, kId);
    const wsrv::Result<void> result = link_->submit_bulk_many(ids, [](wsrv::Session&) {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument)
        << "the memory a task holds is bounded";
}

TEST(BulkAcrossLoops, RecipientsOfDifferentLoopsAreGroupedAndEachLoopGetsItsOwn) {
    wsrv::LinuxSyscalls sys;
    wsrv::SteadyClock clock;
    auto loop_a = wsrv::EventLoop::create(sys, clock, {});
    auto loop_b = wsrv::EventLoop::create(sys, clock, {});
    ASSERT_TRUE(loop_a.has_value() && loop_b.has_value());
    FakeDirectory dir_a;
    FakeDirectory dir_b;
    auto link_a = std::make_shared<wsrv::LoopLink>(**loop_a, dir_a);
    auto link_b = std::make_shared<wsrv::LoopLink>(**loop_b, dir_b);
    RecordingSession one{1};
    RecordingSession two{2};
    RecordingSession three{3};
    dir_a.sessions[1] = &one;
    dir_b.sessions[2] = &two;
    dir_a.sessions[3] = &three;
    const std::vector<SessionHandle> handles{
        SessionHandle{link_a, 1}, SessionHandle{link_b, 2}, SessionHandle{link_a, 3}};
    const auto message = wsrv::SharedMessage::text("hi");
    ASSERT_TRUE(message.has_value());
    const wsrv::BulkSendReport report = wsrv::send_to_many(handles, *message);
    EXPECT_EQ(report.accepted, 3U);
    EXPECT_EQ(report.refused, 0U);
    ASSERT_TRUE((*loop_a)->run_once(0ms).has_value());
    ASSERT_TRUE((*loop_b)->run_once(0ms).has_value());
    EXPECT_EQ(one.texts.size(), 1U);
    EXPECT_EQ(two.texts.size(), 1U);
    EXPECT_EQ(three.texts.size(), 1U);
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
