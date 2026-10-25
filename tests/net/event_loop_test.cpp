// SPDX-License-Identifier: Apache-2.0
#include "net/event_loop.hpp"

#include "net/fd.hpp"
#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using wsrv::ErrorCode;
using wsrv::EventLoop;
using wsrv::EventLoopConfig;
using wsrv::Fd;
using wsrv::IEventHandler;
using wsrv::PollEvents;
using wsrv::WatchHandle;
using wsrv::test::Call;
using wsrv::test::FakeClock;
using wsrv::test::FakeSyscalls;

struct Pair {
    Fd near;
    Fd far;
    Pair() {
        std::array<int, 2> fds{};
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds.data()), 0);
        near.reset(fds[0]);
        far.reset(fds[1]);
    }
    void send(const char* text) const {
        EXPECT_GT(::write(far.get(), text, std::char_traits<char>::length(text)), 0);
    }
    void drain() const {  // edge-triggered discipline: read until EAGAIN
        std::array<char, 256> buf{};
        while (::read(near.get(), buf.data(), buf.size()) > 0) {
        }
    }
};

struct Recorder final : IEventHandler {
    std::vector<PollEvents> seen;
    std::function<void()> action;
    void on_event(PollEvents events) noexcept override {
        seen.push_back(events);
        if (action) {
            action();
        }
    }
};

class EventLoopTest : public ::testing::Test {
protected:
    FakeSyscalls sys;
    FakeClock clock;
    std::unique_ptr<EventLoop> make(EventLoopConfig config = {}) {
        auto created = EventLoop::create(sys, clock, config);
        EXPECT_TRUE(created.has_value());
        return std::move(*created);
    }
};

// ---- tasks -------------------------------------------------------------------------------------

TEST_F(EventLoopTest, AnIdleLoopIterationReturnsImmediately) {
    auto loop = make();
    EXPECT_TRUE(loop->run_once(0ms).has_value());
}

TEST_F(EventLoopTest, PostedTasksRunOnceAndInOrder) {
    auto loop = make();
    std::vector<int> order;
    for (int i = 1; i <= 3; ++i) {
        ASSERT_TRUE(loop->post([&order, i] { order.push_back(i); }).has_value());
    }
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(order.size(), 3U);  // not again
}

TEST_F(EventLoopTest, ATaskPostedByATaskRunsInTheNextIterationNotRecursively) {
    auto loop = make();
    std::vector<int> order;
    ASSERT_TRUE(loop->post([&] {
                    order.push_back(1);
                    EXPECT_TRUE(loop->post([&] { order.push_back(2); }).has_value());
                }).has_value());
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(order, (std::vector<int>{1}));
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST_F(EventLoopTest, TheTaskBudgetCarriesTheRestToTheNextIteration) {  // fairness
    EventLoopConfig config;
    config.max_tasks_per_iteration = 4;
    auto loop = make(config);
    int ran = 0;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(loop->post([&ran] { ++ran; }).has_value());
    }
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(ran, 4);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(ran, 10);
}

TEST_F(EventLoopTest, ThePostQueueIsBoundedAndRecovers) {  // EC-N14
    EventLoopConfig config;
    config.max_queued_tasks = 3;
    auto loop = make(config);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(loop->post([] {}).has_value());
    }
    const auto rejected = loop->post([] {});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, ErrorCode::BufferFull);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_TRUE(loop->post([] {}).has_value());
}

TEST_F(EventLoopTest, AThrowingTaskIsContainedAndCounted) {
    auto loop = make();
    int after = 0;
    ASSERT_TRUE(loop->post([] { throw std::runtime_error("boom"); }).has_value());
    ASSERT_TRUE(loop->post([&after] { ++after; }).has_value());
    EXPECT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(after, 1);
    EXPECT_EQ(loop->task_failures(), 1U);
}

TEST_F(EventLoopTest, APostFromAnotherThreadWakesABlockedLoop) {
    auto loop = make();
    std::atomic<bool> ran{false};
    std::thread runner{[&] { EXPECT_TRUE(loop->run().has_value()); }};
    std::this_thread::sleep_for(50ms);  // let it block in epoll_wait with nothing to do
    ASSERT_TRUE(loop->post([&] {
                    ran = true;
                    loop->stop();
                }).has_value());
    runner.join();
    EXPECT_TRUE(ran);
}

TEST_F(EventLoopTest, StopFromAnotherThreadEndsABlockedRun) {
    auto loop = make();
    std::thread runner{[&] { EXPECT_TRUE(loop->run().has_value()); }};
    std::this_thread::sleep_for(50ms);
    loop->stop();
    runner.join();
}

TEST_F(EventLoopTest, AStopRequestedBeforeRunMakesRunReturnAtOnce) {
    auto loop = make();
    loop->stop();
    EXPECT_TRUE(loop->run().has_value());
}

TEST_F(EventLoopTest, APostMadeBeforeRunStartsIsNotLost) {
    auto loop = make();
    std::atomic<int> ran{0};
    ASSERT_TRUE(loop->post([&] {
                    ++ran;
                    loop->stop();
                }).has_value());
    EXPECT_TRUE(loop->run().has_value());
    EXPECT_EQ(ran, 1);
}

TEST_F(EventLoopTest, ManyThreadsPostingLoseNothingAndRunEachTaskOnce) {  // TSAN
    EventLoopConfig config;
    config.max_queued_tasks = 1U << 20;
    auto loop = make(config);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;
    std::atomic<int> ran{0};
    std::thread runner{[&] { EXPECT_TRUE(loop->run().has_value()); }};
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                while (!loop->post([&ran] { ran.fetch_add(1); }).has_value()) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::thread& p : producers) {
        p.join();
    }
    ASSERT_TRUE(loop->post([&] { loop->stop(); }).has_value());
    runner.join();
    EXPECT_EQ(ran.load(), kThreads * kPerThread);
}

TEST_F(EventLoopTest, AnInterruptedEpollWaitDoesNotStopTheLoop) {  // EC-N01
    sys.fail(Call::EpollWait, EINTR, 2);
    auto loop = make();
    ASSERT_TRUE(loop->post([&] { loop->stop(); }).has_value());
    EXPECT_TRUE(loop->run().has_value());
}

TEST_F(EventLoopTest, AFatalPollerErrorEndsRunWithThatError) {
    auto loop = make();
    sys.fail(Call::EpollWait, EBADF);
    const auto r = loop->run_once(0ms);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Io);
}

// ---- timers ------------------------------------------------------------------------------------

TEST_F(EventLoopTest, ATimerFiresOnceTheClockReachesItsDeadline) {
    auto loop = make();
    int fired = 0;
    ASSERT_TRUE(loop->run_after(50ms, [&fired] { ++fired; }).has_value());
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(fired, 0);
    clock.advance(49ms);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(fired, 0);
    clock.advance(11ms);  // 60 ms: past the 50 ms deadline rounded to the 10 ms tick
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(fired, 1);
    clock.advance(1h);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(fired, 1);
}

TEST_F(EventLoopTest, ACancelledTimerNeverFiresAndCancelIsSafeToRepeat) {
    auto loop = make();
    int fired = 0;
    const auto timer = loop->run_after(20ms, [&fired] { ++fired; });
    ASSERT_TRUE(timer.has_value());
    EXPECT_TRUE(loop->cancel(*timer));
    EXPECT_FALSE(loop->cancel(*timer));
    clock.advance(1s);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(fired, 0);
    EXPECT_FALSE(loop->cancel(wsrv::TimerHandle{}));
}

TEST_F(EventLoopTest, ATimerMayCancelAnotherTimerDueInTheSameIteration) {
    auto loop = make();
    int a = 0;
    int b = 0;
    wsrv::TimerHandle second;
    const auto first = loop->run_after(20ms, [&] {
        ++a;
        loop->cancel(second);
    });
    ASSERT_TRUE(first.has_value());
    const auto later = loop->run_after(20ms, [&b] { ++b; });
    ASSERT_TRUE(later.has_value());
    second = *later;
    clock.advance(30ms);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 0);
}

TEST_F(EventLoopTest, ATimerMayScheduleAnotherTimerAndRearmItself) {
    auto loop = make();
    int fired = 0;
    std::function<void()> tick = [&] {
        if (++fired < 3) {
            EXPECT_TRUE(loop->run_after(10ms, tick).has_value());
        }
    };
    ASSERT_TRUE(loop->run_after(10ms, tick).has_value());
    for (int i = 0; i < 10; ++i) {
        clock.advance(10ms);
        ASSERT_TRUE(loop->run_once(0ms).has_value());
    }
    EXPECT_EQ(fired, 3);
}

TEST_F(EventLoopTest, ATimerStormIsSpreadOverIterationsAndFdEventsAreStillServed) {  // EC-N15
    EventLoopConfig config;
    config.max_timers_per_iteration = 100;
    auto loop = make(config);
    int fired = 0;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(loop->run_after(20ms, [&fired] { ++fired; }).has_value());
    }
    const Pair pair;
    Recorder handler;
    ASSERT_TRUE(loop->watch(pair.near.get(), PollEvents::Read, handler).has_value());
    clock.advance(30ms);
    pair.send("x");
    ASSERT_TRUE(loop->run_once(50ms).has_value());
    EXPECT_EQ(fired, 100);
    EXPECT_EQ(handler.seen.size(), 1U);  // served in the very first iteration, not after the storm
    int iterations = 1;
    while (fired < 1000) {
        ASSERT_TRUE(loop->run_once(0ms).has_value());
        ++iterations;
        ASSERT_LE(iterations, 20);
    }
    EXPECT_EQ(iterations, 10);
}

TEST_F(EventLoopTest, TheTimerBoundIsReported) {
    EventLoopConfig config;
    config.max_timers = 2;
    auto loop = make(config);
    ASSERT_TRUE(loop->run_after(1s, [] {}).has_value());
    ASSERT_TRUE(loop->run_after(1s, [] {}).has_value());
    const auto third = loop->run_after(1s, [] {});
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().code, ErrorCode::BufferFull);
}

// ---- descriptors -------------------------------------------------------------------------------

TEST_F(EventLoopTest, AWatchDeliversReadReadiness) {
    auto loop = make();
    const Pair pair;
    Recorder handler;
    handler.action = [&] { pair.drain(); };
    ASSERT_TRUE(loop->watch(pair.near.get(), PollEvents::Read, handler).has_value());
    pair.send("hello");
    ASSERT_TRUE(loop->run_once(100ms).has_value());
    ASSERT_EQ(handler.seen.size(), 1U);
    EXPECT_TRUE(wsrv::has(handler.seen[0], PollEvents::Read));
}

TEST_F(EventLoopTest, RewatchChangesTheInterest) {
    auto loop = make();
    const Pair pair;
    Recorder handler;
    const auto watch = loop->watch(pair.near.get(), PollEvents::Read, handler);
    ASSERT_TRUE(watch.has_value());
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_TRUE(handler.seen.empty());
    ASSERT_TRUE(loop->rewatch(*watch, PollEvents::Write).has_value());
    ASSERT_TRUE(loop->run_once(100ms).has_value());
    ASSERT_EQ(handler.seen.size(), 1U);
    EXPECT_TRUE(wsrv::has(handler.seen[0], PollEvents::Write));
}

TEST_F(EventLoopTest, UnwatchStopsDeliveryAndASecondUnwatchIsHarmless) {
    auto loop = make();
    const Pair pair;
    Recorder handler;
    const auto watch = loop->watch(pair.near.get(), PollEvents::Read, handler);
    ASSERT_TRUE(watch.has_value());
    loop->unwatch(*watch);
    loop->unwatch(*watch);
    pair.send("x");
    ASSERT_TRUE(loop->run_once(20ms).has_value());
    EXPECT_TRUE(handler.seen.empty());
    EXPECT_FALSE(loop->rewatch(*watch, PollEvents::Read).has_value());  // a dead handle
}

TEST_F(EventLoopTest, TheWatchTableIsBoundedAndFreedSlotsAreReused) {
    EventLoopConfig config;
    config.max_watches = 2;
    auto loop = make(config);
    const Pair a;
    const Pair b;
    const Pair c;
    Recorder handler;
    const auto first = loop->watch(a.near.get(), PollEvents::Read, handler);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(loop->watch(b.near.get(), PollEvents::Read, handler).has_value());
    const auto third = loop->watch(c.near.get(), PollEvents::Read, handler);
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().code, ErrorCode::BufferFull);
    loop->unwatch(*first);
    EXPECT_TRUE(loop->watch(c.near.get(), PollEvents::Read, handler).has_value());
}

TEST_F(EventLoopTest, AWatchOfAnInvalidDescriptorFailsAndLeaksNoSlot) {
    EventLoopConfig config;
    config.max_watches = 1;
    auto loop = make(config);
    Recorder handler;
    EXPECT_FALSE(loop->watch(-1, PollEvents::Read, handler).has_value());
    const Pair pair;
    EXPECT_TRUE(loop->watch(pair.near.get(), PollEvents::Read, handler).has_value());
}

TEST_F(EventLoopTest, AStaleEventForAReusedDescriptorNumberIsNeverDeliveredToTheNewOwner) {  // EC-N12
    auto loop = make();
    Pair a;
    Pair b;
    Recorder handler_a;
    Recorder handler_b;
    Recorder fresh;
    const auto watch_a = loop->watch(a.near.get(), PollEvents::Read, handler_a);
    const auto watch_b = loop->watch(b.near.get(), PollEvents::Read, handler_b);
    ASSERT_TRUE(watch_a.has_value());
    ASSERT_TRUE(watch_b.has_value());
    Pair replacement;  // created later so it cannot take a or b's numbers by accident
    std::optional<WatchHandle> fresh_watch;
    // Whichever handler runs first removes the other watch, closes that socket, and registers a
    // new socket (likely with the closed socket's descriptor number) in the freed slot.
    auto kill_the_other = [&](const WatchHandle& other, Pair& other_pair) {
        loop->unwatch(other);
        other_pair.near.reset();
        const int number = ::dup(replacement.near.get());  // reuses the lowest free number
        fresh_watch = loop->watch(number, PollEvents::Read, fresh).value();
        replacement.near.reset(number);
    };
    bool killed = false;
    handler_a.action = [&] {
        if (!killed) {
            killed = true;
            kill_the_other(*watch_b, b);
        }
    };
    handler_b.action = [&] {
        if (!killed) {
            killed = true;
            kill_the_other(*watch_a, a);
        }
    };
    a.send("x");
    b.send("y");
    ASSERT_TRUE(loop->run_once(100ms).has_value());
    EXPECT_EQ(handler_a.seen.size() + handler_b.seen.size(), 1U);  // the second event was dropped
    EXPECT_TRUE(fresh.seen.empty());                                // and not given to the newcomer
}

TEST_F(EventLoopTest, AHandlerMayUnwatchItsOwnDescriptorFromInsideTheCallback) {
    auto loop = make();
    const Pair pair;
    Recorder handler;
    std::optional<WatchHandle> watch;
    handler.action = [&] { loop->unwatch(*watch); };
    watch = loop->watch(pair.near.get(), PollEvents::Read, handler).value();
    pair.send("x");
    ASSERT_TRUE(loop->run_once(100ms).has_value());
    pair.send("y");
    ASSERT_TRUE(loop->run_once(20ms).has_value());
    EXPECT_EQ(handler.seen.size(), 1U);
}

}  // namespace
