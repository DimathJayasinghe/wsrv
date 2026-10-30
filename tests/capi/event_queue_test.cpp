// SPDX-License-Identifier: Apache-2.0
#include "capi/event_queue.hpp"

#include "net/syscalls.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <poll.h>

namespace {

using namespace wsrv;

HostEvent text(ConnectionId conn, std::size_t size = 1) {
    HostEvent event;
    event.type = HostEventType::Text;
    event.conn = conn;
    event.data.assign(size, std::byte{'x'});
    return event;
}

HostEvent control(HostEventType type, ConnectionId conn) {
    HostEvent event;
    event.type = type;
    event.conn = conn;
    return event;
}

class EventQueueTest : public ::testing::Test {
protected:
    std::unique_ptr<EventQueue> make(EventQueueConfig config) {
        auto queue = EventQueue::create(sys_, config);
        EXPECT_TRUE(queue.has_value());
        return std::move(*queue);
    }

    static bool readable(const EventQueue& queue) {
        pollfd p{queue.event_fd(), POLLIN, 0};
        return ::poll(&p, 1, 0) == 1 && (p.revents & POLLIN) != 0;
    }

    LinuxSyscalls sys_;
};

TEST_F(EventQueueTest, LimitsTooSmallForOneConnectionAreRejectedAtCreation) {
    const auto few_events = EventQueue::create(sys_, {.max_events = 1, .max_bytes = 4096});
    ASSERT_FALSE(few_events.has_value());
    EXPECT_EQ(few_events.error().code, ErrorCode::InvalidConfig);
    const auto few_bytes = EventQueue::create(sys_, {.max_events = 10, .max_bytes = 100});
    ASSERT_FALSE(few_bytes.has_value());
    EXPECT_EQ(few_bytes.error().code, ErrorCode::InvalidConfig);
}

TEST_F(EventQueueTest, AnEmptyQueueHasNothingToPopAndItsDescriptorIsNotReadable) {
    const auto queue = make({});
    EXPECT_FALSE(queue->try_pop().has_value());
    EXPECT_FALSE(readable(*queue));
}

TEST_F(EventQueueTest, APushMakesTheDescriptorReadableAndEmptyingTheQueueClearsIt) {
    const auto queue = make({});
    ASSERT_TRUE(queue->push(text(1)).has_value());
    ASSERT_TRUE(queue->push(text(1)).has_value());
    EXPECT_TRUE(readable(*queue));
    ASSERT_TRUE(queue->try_pop().has_value());
    EXPECT_TRUE(readable(*queue));  // one event is still pending
    ASSERT_TRUE(queue->try_pop().has_value());
    EXPECT_FALSE(readable(*queue));
    EXPECT_FALSE(queue->try_pop().has_value());
}

TEST_F(EventQueueTest, EventsComeOutInPushOrderWithTheirContent) {
    const auto queue = make({});
    ASSERT_TRUE(queue->push(control(HostEventType::Open, 7)).has_value());
    HostEvent first = text(7, 3);
    first.data[0] = std::byte{'a'};
    ASSERT_TRUE(queue->push(std::move(first)).has_value());
    HostEvent close = control(HostEventType::Close, 7);
    close.close_code = 1000;
    close.clean = true;
    ASSERT_TRUE(queue->push(std::move(close)).has_value());

    auto a = queue->try_pop();
    auto b = queue->try_pop();
    auto c = queue->try_pop();
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(a->event.type, HostEventType::Open);
    EXPECT_EQ(b->event.type, HostEventType::Text);
    EXPECT_EQ(b->event.data.size(), 3U);
    EXPECT_EQ(b->event.data[0], std::byte{'a'});
    EXPECT_EQ(c->event.type, HostEventType::Close);
    EXPECT_EQ(c->event.close_code, 1000);
    EXPECT_TRUE(c->event.clean);
}

TEST_F(EventQueueTest, ADataEventOverTheCountLimitIsRefusedAndNothingIsQueued) {
    const auto queue = make({.max_events = 3, .max_bytes = 1024});
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(queue->push(text(1)).has_value());
    }
    const auto refused = queue->push(text(1));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(queue->dropped(), 1U);
    int popped = 0;
    while (queue->try_pop()) {
        ++popped;
    }
    EXPECT_EQ(popped, 3);
}

TEST_F(EventQueueTest, ADataEventOverTheByteLimitIsRefusedEvenWhenTheQueueIsEmpty) {
    const auto queue = make({.max_events = 100, .max_bytes = 1000});
    const auto refused = queue->push(text(1, 1001));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::BufferFull);
    EXPECT_TRUE(queue->push(text(1, 1000)).has_value());
}

TEST_F(EventQueueTest, ACloseAlwaysFitsOnceItsOpenWasAcceptedEvenWhenDataFillsTheQueue) {
    const auto queue = make({.max_events = 4, .max_bytes = 4096});
    ASSERT_TRUE(queue->push(control(HostEventType::Open, 1)).has_value());  // takes 2 slots
    ASSERT_TRUE(queue->push(text(1)).has_value());
    ASSERT_TRUE(queue->push(text(1)).has_value());
    EXPECT_FALSE(queue->push(text(1)).has_value());  // the Close slot is not for data
    EXPECT_TRUE(queue->push(control(HostEventType::Close, 1)).has_value());
}

TEST_F(EventQueueTest, AnOpenIsRefusedWhenItsCloseCouldNotBeReserved) {
    const auto queue = make({.max_events = 3, .max_bytes = 4096});
    ASSERT_TRUE(queue->push(control(HostEventType::Open, 1)).has_value());  // 2 of 3
    EXPECT_FALSE(queue->push(control(HostEventType::Open, 2)).has_value());
    EXPECT_TRUE(queue->push(text(1)).has_value());  // the last free slot is still usable
}

TEST_F(EventQueueTest, ACloseWithoutAnAcceptedOpenIsACallerBug) {
    const auto queue = make({});
    const auto refused = queue->push(control(HostEventType::Close, 9));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

TEST_F(EventQueueTest, AnErrorEventIsBestEffortAndCountedWhenDropped) {
    const auto queue = make({.max_events = 2, .max_bytes = 4096});
    ASSERT_TRUE(queue->push(text(1)).has_value());
    ASSERT_TRUE(queue->push(text(1)).has_value());
    EXPECT_FALSE(queue->push(control(HostEventType::Error, 1)).has_value());
    EXPECT_EQ(queue->dropped(), 1U);
}

TEST_F(EventQueueTest, PressureIsReportedAtHighWaterAndResumeOnceAtLowWater) {
    const auto queue = make({.max_events = 8, .max_bytes = 1 << 20});  // high 6, low 4
    for (int i = 0; i < 5; ++i) {
        const auto pushed = queue->push(text(1));
        ASSERT_TRUE(pushed.has_value());
        EXPECT_FALSE(pushed->pause_reads);
    }
    const auto sixth = queue->push(text(1));
    ASSERT_TRUE(sixth.has_value());
    EXPECT_TRUE(sixth->pause_reads);
    const auto seventh = queue->push(text(1));
    ASSERT_TRUE(seventh.has_value());
    EXPECT_TRUE(seventh->pause_reads);  // still above: every producer is told

    std::vector<bool> resumes;
    while (auto popped = queue->try_pop()) {
        resumes.push_back(popped->resume_reads);
    }
    ASSERT_EQ(resumes.size(), 7U);
    int total = 0;
    for (const bool resume : resumes) {
        total += resume ? 1 : 0;
    }
    EXPECT_EQ(total, 1);      // exactly one notification
    EXPECT_TRUE(resumes[2]);  // after the 3rd pop 4 are left: at or below low water
    EXPECT_FALSE(resumes[1]);
}

TEST_F(EventQueueTest, NoResumeIsReportedWhenPressureNeverArose) {
    const auto queue = make({.max_events = 100, .max_bytes = 1 << 20});
    ASSERT_TRUE(queue->push(text(1)).has_value());
    EXPECT_FALSE(queue->try_pop()->resume_reads);
}

TEST_F(EventQueueTest, ManyProducersOneConsumerLoseNothingKeepEachOrderAndEndNotReadable) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000;
    const auto queue = make({.max_events = 1 << 20, .max_bytes = 64U << 20});
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                HostEvent event = text(static_cast<ConnectionId>(p + 1), 4);
                event.close_code = static_cast<std::uint16_t>(i % 60000);  // sequence number
                event.clean = true;
                ASSERT_TRUE(queue->push(std::move(event)).has_value());
            }
        });
    }
    std::vector<int> next(kProducers, 0);
    int received = 0;
    while (received < kProducers * kPerProducer) {
        if (auto popped = queue->try_pop()) {
            const auto producer = static_cast<std::size_t>(popped->event.conn - 1);
            EXPECT_EQ(popped->event.close_code, next[producer] % 60000);
            ++next[producer];
            ++received;
        }
    }
    for (std::thread& t : producers) {
        t.join();
    }
    EXPECT_FALSE(queue->try_pop().has_value());
    EXPECT_FALSE(readable(*queue));  // no lost or stuck wake-up
}

}  // namespace
