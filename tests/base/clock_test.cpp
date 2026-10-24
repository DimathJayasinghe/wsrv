// SPDX-License-Identifier: Apache-2.0
#include "base/clock.hpp"

#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;  // NOLINT(google-build-using-namespace)

TEST(Clock, SteadyClockNeverGoesBackwards) {
    const wsrv::SteadyClock clock;
    const wsrv::TimePoint first = clock.now();
    const wsrv::TimePoint second = clock.now();
    EXPECT_LE(first, second);
}

TEST(Clock, FakeClockMovesOnlyWhenAdvanced) {
    wsrv::test::FakeClock clock;
    const wsrv::TimePoint start = clock.now();
    EXPECT_EQ(clock.now(), start);
    clock.advance(30s);
    EXPECT_EQ(clock.now() - start, 30s);
    clock.advance(500ms);
    EXPECT_EQ(clock.now() - start, 30500ms);
}

TEST(Clock, ConsumersSeeBothThroughTheInterface) {
    wsrv::test::FakeClock fake;
    const wsrv::SteadyClock real;
    const wsrv::IClock* clocks[] = {&fake, &real};  // NOLINT(modernize-avoid-c-arrays)
    for (const wsrv::IClock* clock : clocks) {
        // Read in sequence: the evaluation order of function arguments is unspecified in C++.
        const wsrv::TimePoint first = clock->now();
        const wsrv::TimePoint second = clock->now();
        EXPECT_LE(first, second);
    }
}

}  // namespace
