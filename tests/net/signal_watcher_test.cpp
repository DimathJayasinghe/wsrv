// SPDX-License-Identifier: Apache-2.0
#include "net/signal_watcher.hpp"

#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::ErrorCode;
using wsrv::EventLoop;
using wsrv::SignalWatcher;
using wsrv::test::Call;
using wsrv::test::FakeClock;
using wsrv::test::FakeSyscalls;

/// The calling thread's current mask, to prove the watcher restores it.
sigset_t current_mask() {
    sigset_t mask;
    sigemptyset(&mask);
    pthread_sigmask(SIG_BLOCK, nullptr, &mask);
    return mask;
}

struct SignalWatcherTest : public ::testing::Test {
    FakeSyscalls sys;
    FakeClock clock;
    std::unique_ptr<EventLoop> loop;
    void SetUp() override {
        auto created = EventLoop::create(sys, clock);
        ASSERT_TRUE(created.has_value());
        loop = std::move(*created);
    }
};

TEST_F(SignalWatcherTest, ASignalBecomesACallbackOnTheLoopThread) {  // EC-N17
    std::vector<int> seen;
    const std::array<int, 2> signals{SIGUSR1, SIGUSR2};
    auto watcher = SignalWatcher::create(*loop, sys, signals, [&](int signo) {
        EXPECT_TRUE(loop->in_loop_thread());
        seen.push_back(signo);
    });
    ASSERT_TRUE(watcher.has_value());
    ASSERT_EQ(::raise(SIGUSR2), 0);  // blocked by the watcher: pending, not delivered as a signal
    ASSERT_EQ(::raise(SIGUSR1), 0);
    ASSERT_TRUE(loop->run_once(1000ms).has_value());
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_NE(seen[0], seen[1]);  // both arrived (order of pending signals is not specified)
}

TEST_F(SignalWatcherTest, AnUnwatchedSignalIsNotReportedAndTheMaskIsRestored) {
    const sigset_t before = current_mask();
    {
        const std::array<int, 1> signals{SIGUSR1};
        auto watcher = SignalWatcher::create(*loop, sys, signals, [](int) {});
        ASSERT_TRUE(watcher.has_value());
        const sigset_t during = current_mask();
        EXPECT_EQ(sigismember(&during, SIGUSR1), 1);
        EXPECT_EQ(sigismember(&during, SIGUSR2), sigismember(&before, SIGUSR2));
    }
    const sigset_t after = current_mask();
    EXPECT_EQ(sigismember(&after, SIGUSR1), sigismember(&before, SIGUSR1));
}

TEST_F(SignalWatcherTest, ASignalStillPendingAtDestructionIsDiscardedNotDelivered) {
    // Unblocking a pending SIGUSR1 would run its default action and kill the process.
    {
        const std::array<int, 1> signals{SIGUSR1};
        auto watcher = SignalWatcher::create(*loop, sys, signals, [](int) {});
        ASSERT_TRUE(watcher.has_value());
        ASSERT_EQ(::raise(SIGUSR1), 0);
    }  // destroyed without running the loop: the test process must survive this line
    SUCCEED();
}

TEST_F(SignalWatcherTest, FailuresAreReportedAndLeaveTheMaskAlone) {
    const sigset_t before = current_mask();
    const std::array<int, 1> signals{SIGUSR1};
    sys.fail(Call::SignalFd, EMFILE);
    auto no_fd = SignalWatcher::create(*loop, sys, signals, [](int) {});
    ASSERT_FALSE(no_fd.has_value());
    EXPECT_EQ(no_fd.error().code, ErrorCode::FdLimit);
    sys.fail(Call::SigMask, EINVAL);
    auto no_mask = SignalWatcher::create(*loop, sys, signals, [](int) {});
    ASSERT_FALSE(no_mask.has_value());
    const sigset_t after = current_mask();
    EXPECT_EQ(sigismember(&after, SIGUSR1), sigismember(&before, SIGUSR1));
}

TEST_F(SignalWatcherTest, ARejectedSignalNumberIsAnError) {
    const std::array<int, 1> bad{-5};
    auto watcher = SignalWatcher::create(*loop, sys, bad, [](int) {});
    ASSERT_FALSE(watcher.has_value());
    EXPECT_EQ(watcher.error().code, ErrorCode::InvalidArgument);
    auto empty = SignalWatcher::create(*loop, sys, std::span<const int>{}, [](int) {});
    ASSERT_FALSE(empty.has_value());
}

}  // namespace
