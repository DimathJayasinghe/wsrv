// SPDX-License-Identifier: Apache-2.0
#include "base/log_rate_limiter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

// ADR-0037 section 5: a token bucket per event name (burst 10, then 1 per second), the held-back
// count is handed to the next line that gets out, and the table of names is bounded.

namespace {

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;

wsrv::TimePoint origin() {
    return wsrv::TimePoint{} + 1000s;
}

}  // namespace

TEST(LogRateLimiter, AllowsABurstOfTenThenHoldsBack) {
    wsrv::LogRateLimiter limiter;
    for (int i = 0; i < 10; ++i) {
        const auto decision = limiter.admit("bad_handshake", origin());
        EXPECT_TRUE(decision.emit) << i;
        EXPECT_EQ(decision.suppressed, 0U);
    }
    EXPECT_FALSE(limiter.admit("bad_handshake", origin()).emit);
    EXPECT_FALSE(limiter.admit("bad_handshake", origin()).emit);
}

TEST(LogRateLimiter, TheNextLineThatGetsOutCarriesTheCountOfTheOnesHeldBack) {
    wsrv::LogRateLimiter limiter;
    for (int i = 0; i < 10; ++i) {
        (void)limiter.admit("flood", origin());
    }
    for (int i = 0; i < 412; ++i) {
        EXPECT_FALSE(limiter.admit("flood", origin()).emit);
    }
    const auto later = limiter.admit("flood", origin() + 1s);
    EXPECT_TRUE(later.emit);
    EXPECT_EQ(later.suppressed, 412U);
    EXPECT_EQ(limiter.admit("flood", origin() + 1s).suppressed, 0U) << "reported once";
}

TEST(LogRateLimiter, RefillsOneLinePerSecond) {
    wsrv::LogRateLimiter limiter;
    for (int i = 0; i < 10; ++i) {
        (void)limiter.admit("e", origin());
    }
    EXPECT_FALSE(limiter.admit("e", origin() + 999ms).emit);
    EXPECT_TRUE(limiter.admit("e", origin() + 1000ms).emit);
    EXPECT_FALSE(limiter.admit("e", origin() + 1500ms).emit);
    EXPECT_TRUE(limiter.admit("e", origin() + 2000ms).emit);
}

TEST(LogRateLimiter, AnIdleHourBuysABurstNotAnHourOfLines) {
    wsrv::LogRateLimiter limiter;
    for (int i = 0; i < 10; ++i) {
        (void)limiter.admit("e", origin());
    }
    int emitted = 0;
    for (int i = 0; i < 50; ++i) {
        emitted += limiter.admit("e", origin() + 3600s).emit ? 1 : 0;
    }
    EXPECT_EQ(emitted, 10);
}

TEST(LogRateLimiter, EveryEventHasItsOwnBucket) {
    wsrv::LogRateLimiter limiter;
    for (int i = 0; i < 10; ++i) {
        (void)limiter.admit("noisy", origin());
    }
    EXPECT_FALSE(limiter.admit("noisy", origin()).emit);
    EXPECT_TRUE(limiter.admit("rare", origin()).emit)
        << "a flood of one event does not hide another";
}

TEST(LogRateLimiter, TheTableIsBoundedAndEventsBeyondItShareOneBucket) {
    wsrv::LogRateLimiter limiter;
    for (std::size_t i = 0; i < wsrv::LogRateLimiter::kMaxEvents; ++i) {
        EXPECT_TRUE(limiter.admit("event" + std::to_string(i), origin()).emit);
    }
    // 10000 different names (a bug, or a peer-chosen name) must not grow anything: together they
    // get one burst, not ten each.
    int emitted = 0;
    for (int i = 0; i < 10'000; ++i) {
        emitted += limiter.admit("overflow" + std::to_string(i), origin()).emit ? 1 : 0;
    }
    EXPECT_LE(emitted, 10);
}

TEST(LogRateLimiter, ATimeThatGoesBackwardsDoesNotGiveOrTakeTokens) {
    wsrv::LogRateLimiter limiter;
    for (int i = 0; i < 10; ++i) {
        (void)limiter.admit("e", origin());
    }
    EXPECT_FALSE(limiter.admit("e", origin() - 5s).emit);
}
