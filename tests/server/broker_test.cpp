// SPDX-License-Identifier: Apache-2.0
// The pub/sub broker (ADR-0024): who gets what, how names and sizes are bounded, how one slow or
// gone subscriber leaves the others alone, and that nothing leaks. A real EventLoop pumped by the
// test thread and recording sessions stand in for connections, so every outcome is deterministic.
#include "server/broker.hpp"

#include "base/clock.hpp"
#include "base/metrics.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "support/recording_session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::Broker;
using wsrv::BrokerConfig;
using wsrv::BrokerMembership;
using wsrv::ConnectionId;
using wsrv::ErrorCode;
using wsrv::SessionHandle;
using wsrv::test::FakeDirectory;
using wsrv::test::RecordingSession;

class BrokerTest : public ::testing::Test {
protected:
    explicit BrokerTest(std::size_t max_tasks = 4096) {
        wsrv::EventLoopConfig config;
        config.max_queued_tasks = max_tasks;
        auto loop = wsrv::EventLoop::create(sys_, clock_, config);
        EXPECT_TRUE(loop.has_value());
        loop_ = std::move(*loop);
        link_ = std::make_shared<wsrv::LoopLink>(*loop_, directory_);
    }

    /// A subscriber-to-be: a session registered under `id`, and its handle.
    SessionHandle add(ConnectionId id) {
        sessions_[id] = std::make_unique<RecordingSession>(id);
        directory_.sessions[id] = sessions_[id].get();
        return SessionHandle{link_, id};
    }
    RecordingSession& session(ConnectionId id) {
        return *sessions_.at(id);
    }

    void pump(std::chrono::milliseconds quiet = 30ms) {
        const auto until = std::chrono::steady_clock::now() + quiet;
        while (std::chrono::steady_clock::now() < until) {
            EXPECT_TRUE(loop_->run_once(5ms).has_value());
        }
    }

    static inline wsrv::LinuxSyscalls sys_;
    static inline wsrv::SteadyClock clock_;
    FakeDirectory directory_;
    std::map<ConnectionId, std::unique_ptr<RecordingSession>> sessions_;
    std::unique_ptr<wsrv::EventLoop> loop_;
    std::shared_ptr<wsrv::LoopLink> link_;
};

class SmallQueueBrokerTest : public BrokerTest {
protected:
    SmallQueueBrokerTest() : BrokerTest{4} {}
};

BrokerConfig tiny(std::size_t topics = 4,
                  std::size_t per_topic = 4,
                  std::size_t per_connection = 3,
                  std::size_t total = 8) {
    BrokerConfig config;
    config.max_topics = topics;
    config.max_subscribers_per_topic = per_topic;
    config.max_topics_per_connection = per_connection;
    config.max_subscriptions = total;
    return config;
}

// ---- delivery ----------------------------------------------------------------------------------

TEST_F(BrokerTest, APublishReachesTheSubscribersOfThatTopicAndNobodyElse) {
    Broker broker;
    ASSERT_TRUE(broker.subscribe(add(1), "news").has_value());
    ASSERT_TRUE(broker.subscribe(add(2), "news").has_value());
    ASSERT_TRUE(broker.subscribe(add(3), "sports").has_value());
    const auto report = broker.publish_text("news", "hello");
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->subscribers, 2U);
    EXPECT_EQ(report->accepted, 2U);
    EXPECT_EQ(report->refused, 0U);
    pump();
    EXPECT_EQ(session(1).texts, std::vector<std::string>{"hello"});
    EXPECT_EQ(session(2).texts, std::vector<std::string>{"hello"});
    EXPECT_TRUE(session(3).texts.empty());
}

TEST_F(BrokerTest, BinaryMessagesArriveAsBinary) {
    Broker broker;
    ASSERT_TRUE(broker.subscribe(add(1), "bytes").has_value());
    const std::string raw{"\x00\x01\xFF", 3};  // the length, or the NUL ends the literal
    ASSERT_TRUE(broker.publish_binary("bytes", wsrv::as_bytes(raw)).has_value());
    pump();
    ASSERT_EQ(session(1).binaries.size(), 1U);
    EXPECT_EQ(session(1).binaries[0], std::string("\x00\x01\xFF", 3));
    EXPECT_TRUE(session(1).texts.empty());
}

TEST_F(BrokerTest, ThePublisherCanBeLeftOutOfItsOwnBroadcast) {  // the chat case
    Broker broker;
    ASSERT_TRUE(broker.subscribe(add(1), "room").has_value());
    ASSERT_TRUE(broker.subscribe(add(2), "room").has_value());
    const auto report = broker.publish_text("room", "from 1", /*except=*/1);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->subscribers, 1U);
    pump();
    EXPECT_TRUE(session(1).texts.empty());
    EXPECT_EQ(session(2).texts.size(), 1U);
}

TEST_F(BrokerTest, MessagesFromOnePublisherArriveInOrder) {
    Broker broker;
    ASSERT_TRUE(broker.subscribe(add(1), "t").has_value());
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(broker.publish_text("t", std::to_string(i)).has_value());
    }
    pump(100ms);
    ASSERT_EQ(session(1).texts.size(), 200U);
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(session(1).texts[static_cast<std::size_t>(i)], std::to_string(i));
    }
}

TEST_F(BrokerTest, PublishingToATopicNobodyListensToDeliversToNobodyAndCreatesNothing) {
    Broker broker;
    const auto report = broker.publish_text("empty", "x");
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->subscribers, 0U);
    EXPECT_EQ(broker.topic_count(), 0U) << "a publish must not create state a peer can fill";
}

TEST_F(BrokerTest, TextThatIsNotUtf8IsRefusedOnceAndNothingIsSent) {
    Broker broker;
    ASSERT_TRUE(broker.subscribe(add(1), "t").has_value());
    const auto report = broker.publish_text("t", "\xC3\x28");
    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(report.error().code, ErrorCode::InvalidUtf8);
    pump();
    EXPECT_TRUE(session(1).texts.empty());
}

// ---- subscriptions -----------------------------------------------------------------------------

TEST_F(BrokerTest, SubscribingTwiceIsOneSubscription) {
    Broker broker;
    const SessionHandle handle = add(1);
    ASSERT_TRUE(broker.subscribe(handle, "t").has_value());
    ASSERT_TRUE(broker.subscribe(handle, "t").has_value());
    EXPECT_EQ(broker.subscriber_count("t"), 1U);
    ASSERT_TRUE(broker.publish_text("t", "once").has_value());
    pump();
    EXPECT_EQ(session(1).texts.size(), 1U);
}

TEST_F(BrokerTest, ASecondSubscribeDoesNotUseUpAnotherSlotOfTheTotal) {
    Broker broker{tiny(/*topics=*/10, /*per_topic=*/10, /*per_connection=*/10, /*total=*/2)};
    const SessionHandle one = add(1);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(broker.subscribe(one, "t").has_value());
    }
    EXPECT_TRUE(broker.subscribe(add(2), "t").has_value()) << "the repeats were not counted";
    EXPECT_FALSE(broker.subscribe(add(3), "t").has_value());
}

TEST_F(BrokerTest, UnsubscribeStopsDeliveryAndTheLastOneRemovesTheTopic) {
    Broker broker;
    const SessionHandle handle = add(1);
    ASSERT_TRUE(broker.subscribe(handle, "t").has_value());
    broker.unsubscribe(1, "t");
    EXPECT_EQ(broker.subscriber_count("t"), 0U);
    EXPECT_EQ(broker.topic_count(), 0U) << "empty topics are erased: bounded memory";
    ASSERT_TRUE(broker.publish_text("t", "x").has_value());
    pump();
    EXPECT_TRUE(session(1).texts.empty());
    broker.unsubscribe(1, "t");  // again, and for a topic that never existed: harmless
    broker.unsubscribe(99, "nothing");
}

TEST_F(BrokerTest, UnsubscribeAllRemovesEveryTopicOfOneConnectionOnly) {
    Broker broker;
    const SessionHandle one = add(1);
    const SessionHandle two = add(2);
    for (const char* topic : {"a", "b", "c"}) {
        ASSERT_TRUE(broker.subscribe(one, topic).has_value());
    }
    ASSERT_TRUE(broker.subscribe(two, "b").has_value());
    broker.unsubscribe_all(1);
    EXPECT_EQ(broker.subscriber_count("a"), 0U);
    EXPECT_EQ(broker.subscriber_count("b"), 1U);
    EXPECT_EQ(broker.topic_count(), 1U);
    broker.unsubscribe_all(1);  // idempotent
}

TEST_F(BrokerTest, AMembershipGivesEverythingBackWhenItIsDestroyed) {  // ADR-0024 section 5
    Broker broker;
    {
        BrokerMembership member{broker};
        ASSERT_TRUE(member.subscribe(add(1), "a").has_value());
        ASSERT_TRUE(member.subscribe(add(1), "b").has_value());
        EXPECT_EQ(broker.topic_count(), 2U);
    }
    EXPECT_EQ(broker.topic_count(), 0U);
    {
        const BrokerMembership never_used{broker};  // nothing subscribed: nothing to undo
    }
    EXPECT_EQ(broker.topic_count(), 0U);
}

TEST_F(BrokerTest, AMembershipLeavesASingleTopicOnRequest) {
    Broker broker;
    BrokerMembership member{broker};
    ASSERT_TRUE(member.subscribe(add(1), "a").has_value());
    ASSERT_TRUE(member.subscribe(add(1), "b").has_value());
    member.leave("a");
    EXPECT_EQ(broker.subscriber_count("a"), 0U);
    EXPECT_EQ(broker.subscriber_count("b"), 1U);
}

// ---- names and bounds --------------------------------------------------------------------------

TEST_F(BrokerTest, OnlyPlainTopicNamesAreAccepted) {  // peer-chosen text, ADR-0024 section 2
    Broker broker;
    const SessionHandle handle = add(1);
    for (const char* good : {"a", "room-1", "chat.general", "a/b:c@d_e", "ABC123", "x.y-z"}) {
        EXPECT_TRUE(broker.subscribe(handle, good).has_value()) << good;
    }
    const std::string with_nul{"a\0b", 3};
    const std::string too_long(129, 'a');
    for (const std::string& bad : {std::string{},
                                   std::string{"has space"},
                                   std::string{"tab\t"},
                                   std::string{"new\nline"},
                                   std::string{"caf\xC3\xA9"},
                                   std::string{"wild*card"},
                                   std::string{"hash#"},
                                   std::string{"quo\"te"},
                                   with_nul,
                                   too_long}) {
        const auto result = broker.subscribe(handle, bad);
        ASSERT_FALSE(result.has_value()) << "'" << bad << "'";
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(broker.publish_text("bad name", "x").has_value()) << "publish checks the name too";
    EXPECT_EQ(broker.topic_count(), 6U);
    const std::string longest(128, 'a');
    EXPECT_TRUE(broker.subscribe(handle, longest).has_value()) << "128 is the limit, inclusive";
}

TEST_F(BrokerTest, EachBoundRefusesTheNextSubscriptionAndChangesNothing) {  // overflow actions
    Broker broker{tiny(/*topics=*/3, /*per_topic=*/2, /*per_connection=*/2, /*total=*/100)};
    const SessionHandle a = add(1);
    const SessionHandle b = add(2);
    const SessionHandle c = add(3);
    ASSERT_TRUE(broker.subscribe(a, "t1").has_value());
    ASSERT_TRUE(broker.subscribe(a, "t2").has_value());
    auto third = broker.subscribe(a, "t3");  // per connection: 2
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().code, ErrorCode::BufferFull);
    ASSERT_TRUE(broker.subscribe(b, "t1").has_value());
    auto crowded = broker.subscribe(c, "t1");  // per topic: 2
    ASSERT_FALSE(crowded.has_value());
    EXPECT_EQ(crowded.error().code, ErrorCode::BufferFull);
    ASSERT_TRUE(broker.subscribe(b, "t3").has_value());  // third distinct topic: the limit
    auto fourth = broker.subscribe(c, "t4");             // topics: 3
    ASSERT_FALSE(fourth.has_value());
    EXPECT_EQ(fourth.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(broker.topic_count(), 3U);
    EXPECT_EQ(broker.subscriber_count("t4"), 0U);
}

TEST_F(BrokerTest, TheTotalBoundStopsAForgottenUnsubscribeFromGrowingForever) {
    Broker broker{tiny(/*topics=*/100, /*per_topic=*/100, /*per_connection=*/100, /*total=*/5)};
    for (ConnectionId id = 1; id <= 5; ++id) {
        ASSERT_TRUE(broker.subscribe(add(id), "t").has_value());
    }
    const auto over = broker.subscribe(add(6), "t");
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code, ErrorCode::BufferFull);
    broker.unsubscribe_all(3);
    EXPECT_TRUE(broker.subscribe(add(6), "t").has_value()) << "a freed slot is usable again";
}

TEST_F(BrokerTest, AnOversizedPayloadIsRefused) {
    BrokerConfig config;
    config.max_payload_bytes = 16;
    Broker broker{config};
    ASSERT_TRUE(broker.subscribe(add(1), "t").has_value());
    EXPECT_TRUE(broker.publish_text("t", std::string(16, 'a')).has_value());
    const auto big = broker.publish_text("t", std::string(17, 'a'));
    ASSERT_FALSE(big.has_value());
    EXPECT_EQ(big.error().code, ErrorCode::InvalidArgument);
    const std::string raw(17, 'b');
    EXPECT_FALSE(broker.publish_binary("t", wsrv::as_bytes(raw)).has_value());
}

// ---- isolation ---------------------------------------------------------------------------------

TEST_F(SmallQueueBrokerTest, ARefusingLoopCostsOnlyThoseDeliveriesAndIsReportedNotHidden) {
    Broker broker;
    for (ConnectionId id = 1; id <= 8; ++id) {
        ASSERT_TRUE(broker.subscribe(add(id), "t").has_value());
    }
    ASSERT_TRUE(broker.subscribe(add(99), "other").has_value());
    for (int i = 0; i < 3; ++i) {  // the loop holds 4 tasks, 3 for broadcasts: use them up
        ASSERT_TRUE(broker.publish_text("other", "filler").has_value());
    }
    const auto report = broker.publish_text("t", "x");  // not pumped yet: no slot for the chunk
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->subscribers, 8U);
    EXPECT_EQ(report->accepted, 0U) << "a chunk is taken or refused whole (ADR-0034)";
    EXPECT_EQ(report->refused, 8U) << "counted per recipient, and reported, not hidden";
    pump();
    std::size_t received = 0;
    for (ConnectionId id = 1; id <= 8; ++id) {
        received += session(id).texts.size();
    }
    EXPECT_EQ(received, report->accepted) << "exactly the accepted ones were delivered";
    const auto again = broker.publish_text("t", "y");  // the queue has room again
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->accepted, 8U) << "the whole room is one chunk: one slot";
}

TEST_F(SmallQueueBrokerTest, ARefusedTailIsNotAlwaysTheSameSubscribers) {  // fairness, ADR-0024 A1
    // Four chunks of recipients, a lane of three: one chunk is refused each time. The start of the
    // order moves by a chunk per publish, so every subscriber is the refused one now and then.
    constexpr ConnectionId kSubscribers = wsrv::kBulkChunk * 4;
    Broker broker;
    for (ConnectionId id = 1; id <= kSubscribers; ++id) {
        ASSERT_TRUE(broker.subscribe(add(id), "t").has_value());
    }
    for (int round = 0; round < 4; ++round) {
        const auto report = broker.publish_text("t", "x");
        ASSERT_TRUE(report.has_value());
        EXPECT_EQ(report->accepted, wsrv::kBulkChunk * 3);
        EXPECT_EQ(report->refused, wsrv::kBulkChunk);
        pump();  // drain, so every publish meets an empty queue
    }
    for (ConnectionId id = 1; id <= kSubscribers; ++id) {
        EXPECT_EQ(session(id).texts.size(), 3U)
            << "subscriber " << id
            << " must be refused once in four publishes, not always or never";
    }
}

TEST_F(SmallQueueBrokerTest, AFloodOfBroadcastsLeavesRoomForAClose) {
    Broker broker;
    const SessionHandle victim = add(1);
    ASSERT_TRUE(broker.subscribe(victim, "t").has_value());
    for (int i = 0; i < 10; ++i) {
        static_cast<void>(broker.publish_text("t", "x").has_value());
    }
    EXPECT_TRUE(victim.close(wsrv::CloseCode::GoingAway, "bye").has_value());
}

TEST_F(BrokerTest, ASubscriberWhoseServerIsGoneIsDroppedAndTheRestOfTheTableIsKept) {
    Broker broker;
    ASSERT_TRUE(broker.subscribe(add(1), "t").has_value());
    link_->close();  // the loop and its connections are gone: every send says Closing
    const auto report = broker.publish_text("t", "x");
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->refused, 1U);
    EXPECT_EQ(broker.subscriber_count("t"), 0U) << "a Closing answer removes the subscription";
    EXPECT_EQ(broker.topic_count(), 0U);
}

// ---- metrics -----------------------------------------------------------------------------------

TEST_F(BrokerTest, PublishesAndDeliveriesAreCounted) {  // ADR-0024 section 6
    const auto bank = std::make_shared<wsrv::CounterBank>();
    Broker broker{BrokerConfig{}, bank};
    ASSERT_TRUE(broker.subscribe(add(1), "t").has_value());
    ASSERT_TRUE(broker.subscribe(add(2), "t").has_value());
    ASSERT_TRUE(broker.publish_text("t", "a").has_value());
    ASSERT_TRUE(broker.publish_text("t", "b").has_value());
    EXPECT_EQ(bank->read(wsrv::Metric::BrokerPublishes), 2U);
    EXPECT_EQ(bank->read(wsrv::Metric::BrokerDeliveriesAccepted), 4U);
    EXPECT_EQ(bank->read(wsrv::Metric::BrokerDeliveriesRefused), 0U);
    EXPECT_FALSE(broker.publish_text("t", "\xC3\x28").has_value());
    EXPECT_EQ(bank->read(wsrv::Metric::BrokerPublishes), 2U) << "a refused publish is not counted";
}

TEST_F(BrokerTest, TheBrokerKeepsItsCountersAliveAfterTheirOwnerIsGone) {  // review finding 4
    std::shared_ptr<wsrv::CounterBank> kept;
    Broker broker;
    {
        wsrv::MetricBanks banks{1};
        kept = banks.shared();
        broker.use_metrics(kept);
    }  // the banks (the server's) are destroyed; a publish after this must not touch freed memory
    ASSERT_TRUE(broker.subscribe(add(1), "t").has_value());
    ASSERT_TRUE(broker.publish_text("t", "x").has_value());
    EXPECT_EQ(kept->read(wsrv::Metric::BrokerPublishes), 1U);
}

// ---- threads -----------------------------------------------------------------------------------

TEST_F(BrokerTest, SubscribePublishAndUnsubscribeFromManyThreadsLeaveNothingBehind) {  // TSan
    Broker broker;
    constexpr int kThreads = 4;
    std::vector<SessionHandle> handles;
    for (ConnectionId id = 1; id <= kThreads; ++id) {
        handles.push_back(add(id));
    }
    std::atomic<bool> stop{false};
    std::thread pumper([&] {
        while (!stop.load()) {
            static_cast<void>(loop_->run_once(1ms).has_value());
        }
    });
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            const SessionHandle& me = handles[static_cast<std::size_t>(t)];
            for (int i = 0; i < 300; ++i) {
                const std::string topic = "room" + std::to_string(i % 5);
                static_cast<void>(broker.subscribe(me, topic).has_value());
                static_cast<void>(broker.publish_text(topic, "m").has_value());
                broker.unsubscribe(me.id(), topic);
            }
            broker.unsubscribe_all(me.id());
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    stop.store(true);
    pumper.join();
    EXPECT_EQ(broker.topic_count(), 0U);
}

}  // namespace
