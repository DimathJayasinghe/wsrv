// SPDX-License-Identifier: Apache-2.0
#include "net/timer_wheel.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::ErrorCode;
using wsrv::TimerId;
using wsrv::TimerWheel;
using wsrv::TimerWheelConfig;
using wsrv::TimePoint;

const TimePoint kStart{};

TimerWheelConfig small_config(std::size_t slots = 8, std::size_t max_timers = 1000) {
    TimerWheelConfig c;
    c.tick = 10ms;
    c.slots = slots;
    c.max_timers = max_timers;
    return c;
}

std::vector<TimerId> advance(TimerWheel& wheel, TimePoint now, std::size_t max = SIZE_MAX) {
    std::vector<TimerId> out;
    wheel.advance(now, out, max);
    return out;
}

TEST(TimerWheel, AnEmptyWheelHasNothingToDo) {
    TimerWheel wheel{small_config(), kStart};
    EXPECT_EQ(wheel.size(), 0U);
    EXPECT_FALSE(wheel.next_wake().has_value());
    EXPECT_TRUE(advance(wheel, kStart + 1h).empty());
}

TEST(TimerWheel, ATimerFiresAtItsDeadlineTickAndNotBefore) {
    TimerWheel wheel{small_config(), kStart};
    const auto id = wheel.schedule(kStart + 50ms);
    ASSERT_TRUE(id.has_value());
    EXPECT_NE(*id, 0U);
    EXPECT_EQ(wheel.size(), 1U);
    EXPECT_TRUE(advance(wheel, kStart + 49ms).empty());
    EXPECT_EQ(advance(wheel, kStart + 50ms), (std::vector<TimerId>{*id}));
    EXPECT_EQ(wheel.size(), 0U);
    EXPECT_TRUE(advance(wheel, kStart + 1h).empty());  // fires once
}

TEST(TimerWheel, ADeadlineBetweenTicksFiresAtTheNextTickBoundaryNeverEarly) {
    TimerWheel wheel{small_config(), kStart};
    const auto id = wheel.schedule(kStart + 51ms);
    EXPECT_TRUE(advance(wheel, kStart + 59ms).empty());
    EXPECT_EQ(advance(wheel, kStart + 60ms), (std::vector<TimerId>{*id}));
}

TEST(TimerWheel, ATimerBeyondOneRevolutionWaitsForItsRound) {
    TimerWheel wheel{small_config(8), kStart};  // one revolution = 80 ms
    const auto id = wheel.schedule(kStart + 250ms);
    EXPECT_TRUE(advance(wheel, kStart + 80ms).empty());   // same slot, earlier round
    EXPECT_TRUE(advance(wheel, kStart + 160ms).empty());
    EXPECT_TRUE(advance(wheel, kStart + 249ms).empty());
    EXPECT_EQ(advance(wheel, kStart + 250ms), (std::vector<TimerId>{*id}));
}

TEST(TimerWheel, ADeadlineInThePastFiresOnTheNextAdvance) {
    TimerWheel wheel{small_config(), kStart + 1s};
    advance(wheel, kStart + 1s);
    const auto id = wheel.schedule(kStart + 10ms);
    EXPECT_EQ(advance(wheel, kStart + 1s), (std::vector<TimerId>{*id}));  // not even a tick later
}

TEST(TimerWheel, CancelPreventsFiringAndIsHarmlessTwiceOrForUnknownIds) {
    TimerWheel wheel{small_config(), kStart};
    const auto a = wheel.schedule(kStart + 30ms);
    const auto b = wheel.schedule(kStart + 30ms);
    EXPECT_TRUE(wheel.cancel(*a));
    EXPECT_FALSE(wheel.cancel(*a));
    EXPECT_FALSE(wheel.cancel(987654));
    EXPECT_FALSE(wheel.cancel(0));
    EXPECT_EQ(advance(wheel, kStart + 30ms), (std::vector<TimerId>{*b}));
    EXPECT_FALSE(wheel.cancel(*b));  // already fired
    EXPECT_EQ(wheel.size(), 0U);
}

TEST(TimerWheel, IdsAreNeverReused) {
    TimerWheel wheel{small_config(), kStart};
    std::set<TimerId> seen;
    for (int i = 0; i < 100; ++i) {
        const auto id = wheel.schedule(kStart + 10ms);
        ASSERT_TRUE(id.has_value());
        EXPECT_TRUE(seen.insert(*id).second);
        EXPECT_TRUE(wheel.cancel(*id));
    }
}

TEST(TimerWheel, TimersFireEarliestFirstAndTiesByCreationOrder) {
    TimerWheel wheel{small_config(), kStart};
    const auto late = wheel.schedule(kStart + 70ms);
    const auto first = wheel.schedule(kStart + 20ms);
    const auto second = wheel.schedule(kStart + 20ms);
    const auto mid = wheel.schedule(kStart + 40ms);
    EXPECT_EQ(advance(wheel, kStart + 1s),
              (std::vector<TimerId>{*first, *second, *mid, *late}));
}

TEST(TimerWheel, ABigClockJumpFiresEverythingDue) {
    TimerWheel wheel{small_config(8), kStart};
    std::vector<TimerId> ids;
    for (int i = 1; i <= 50; ++i) {
        ids.push_back(*wheel.schedule(kStart + std::chrono::milliseconds{i * 37}));
    }
    EXPECT_EQ(advance(wheel, kStart + 24h), ids);  // all, in deadline order, in one call
}

TEST(TimerWheel, TheTimerBoundIsEnforcedAndRecovers) {  // EC-N14 style bound
    TimerWheel wheel{small_config(8, 3), kStart};
    ASSERT_TRUE(wheel.schedule(kStart + 10ms).has_value());
    ASSERT_TRUE(wheel.schedule(kStart + 10ms).has_value());
    const auto third = wheel.schedule(kStart + 10ms);
    ASSERT_TRUE(third.has_value());
    const auto fourth = wheel.schedule(kStart + 10ms);
    ASSERT_FALSE(fourth.has_value());
    EXPECT_EQ(fourth.error().code, ErrorCode::BufferFull);
    EXPECT_TRUE(wheel.cancel(*third));
    EXPECT_TRUE(wheel.schedule(kStart + 10ms).has_value());
}

TEST(TimerWheel, AFireBudgetLeavesTheRestDueForTheNextCall) {  // EC-N15
    TimerWheel wheel{small_config(), kStart};
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(wheel.schedule(kStart + 20ms).has_value());
    }
    EXPECT_EQ(advance(wheel, kStart + 20ms, 4).size(), 4U);
    EXPECT_EQ(wheel.size(), 6U);
    EXPECT_EQ(advance(wheel, kStart + 20ms, 4).size(), 4U);
    EXPECT_EQ(advance(wheel, kStart + 20ms, 4).size(), 2U);
    EXPECT_EQ(wheel.size(), 0U);
}

TEST(TimerWheel, TimeNeverMovesBackwards) {
    TimerWheel wheel{small_config(), kStart};
    advance(wheel, kStart + 100ms);
    const auto id = wheel.schedule(kStart + 150ms);
    EXPECT_TRUE(advance(wheel, kStart + 10ms).empty());  // an earlier clock reading is ignored
    EXPECT_EQ(advance(wheel, kStart + 150ms), (std::vector<TimerId>{*id}));
}

TEST(TimerWheel, NextWakeIsNeverLaterThanTheEarliestDeadline) {
    TimerWheel wheel{small_config(8), kStart};
    ASSERT_TRUE(wheel.schedule(kStart + 700ms).has_value());
    ASSERT_TRUE(wheel.next_wake().has_value());
    EXPECT_LE(*wheel.next_wake(), kStart + 700ms);
    const auto soon = wheel.schedule(kStart + 35ms);
    EXPECT_LE(*wheel.next_wake(), kStart + 40ms);  // the deadline tick
    EXPECT_GT(*wheel.next_wake(), kStart);          // and it is not "right now"
    EXPECT_TRUE(wheel.cancel(*soon));
    EXPECT_LE(*wheel.next_wake(), kStart + 700ms);
}

TEST(TimerWheel, NextWakeIsNowWhenSomethingIsAlreadyDue) {
    TimerWheel wheel{small_config(), kStart};
    advance(wheel, kStart + 1s);
    ASSERT_TRUE(wheel.schedule(kStart + 5ms).has_value());
    ASSERT_TRUE(wheel.next_wake().has_value());
    EXPECT_LE(*wheel.next_wake(), kStart + 1s);
}

TEST(TimerWheel, ADeadlineFarBeyondAnyClockReadingDoesNotOverflow) {
    TimerWheel wheel{small_config(), kStart};
    const auto id = wheel.schedule(TimePoint::max());
    ASSERT_TRUE(id.has_value());
    EXPECT_TRUE(advance(wheel, kStart + 24h * 365).empty());
    EXPECT_TRUE(wheel.cancel(*id));
}

// ---- model test: the wheel must agree with a naive sorted reference --------------------------

TEST(TimerWheel, AgreesWithANaiveModelOverTwentyThousandRandomOperations) {
    const TimerWheelConfig config = small_config(16, 1U << 20);
    TimerWheel wheel{config, kStart};
    std::mt19937_64 rng{20261024};
    // The model: id -> boundary (deadline rounded UP to a tick). A timer is due when the clock's
    // tick (rounded DOWN) has reached its boundary.
    std::map<TimerId, std::int64_t> model;
    std::set<TimerId> live;
    std::int64_t now_ms = 0;
    auto boundary = [&](std::int64_t deadline_ms) {
        return (deadline_ms + 9) / 10 * 10;
    };
    for (int step = 0; step < 20000; ++step) {
        switch (rng() % 5) {
        case 0:
        case 1: {  // schedule, sometimes in the past, sometimes far away
            const std::int64_t deadline = now_ms + static_cast<std::int64_t>(rng() % 3000) - 200;
            const auto id = wheel.schedule(kStart + std::chrono::milliseconds{std::max<std::int64_t>(deadline, 0)});
            ASSERT_TRUE(id.has_value());
            model[*id] = boundary(std::max<std::int64_t>(deadline, 0));
            live.insert(*id);
            break;
        }
        case 2: {  // cancel something live, or something bogus
            if (!live.empty() && rng() % 4 != 0) {
                auto it = live.begin();
                std::advance(it, static_cast<std::ptrdiff_t>(rng() % live.size()));
                EXPECT_TRUE(wheel.cancel(*it));
                model.erase(*it);
                live.erase(it);
            } else {
                EXPECT_FALSE(wheel.cancel(rng() | (1ULL << 40)));
            }
            break;
        }
        default: {  // advance: small steps mostly, occasionally a big jump
            now_ms += (rng() % 10 == 0) ? static_cast<std::int64_t>(rng() % 5000) : static_cast<std::int64_t>(rng() % 40);
            const std::vector<TimerId> fired = advance(wheel, kStart + std::chrono::milliseconds{now_ms});
            const std::int64_t tick_floor = now_ms / 10 * 10;
            std::vector<TimerId> expected;
            for (const auto& [id, b] : model) {
                if (b <= tick_floor) {
                    expected.push_back(id);
                }
            }
            std::vector<TimerId> sorted_fired = fired;
            std::sort(sorted_fired.begin(), sorted_fired.end());
            ASSERT_EQ(sorted_fired, expected) << "step " << step;
            std::int64_t last = -1;  // earliest boundary first
            for (const TimerId id : fired) {
                EXPECT_GE(model.at(id), last);
                last = model.at(id);
            }
            for (const TimerId id : fired) {
                model.erase(id);
                live.erase(id);
            }
            break;
        }
        }
        ASSERT_EQ(wheel.size(), model.size()) << "step " << step;
        if (!model.empty()) {
            const std::int64_t earliest = std::min_element(model.begin(), model.end(), [](const auto& l, const auto& r) { return l.second < r.second; })->second;
            ASSERT_TRUE(wheel.next_wake().has_value());
            EXPECT_LE(*wheel.next_wake(), kStart + std::chrono::milliseconds{std::max(earliest, now_ms / 10 * 10)}) << "step " << step;
        }
    }
}

}  // namespace
