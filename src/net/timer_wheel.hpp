// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wsrv {

/// Identifies one scheduled timer. 0 is never a valid id and ids are never reused, so a stale id
/// held by a connection that already fired or cancelled it can never touch somebody else's timer.
using TimerId = std::uint64_t;

struct TimerWheelConfig {
    std::chrono::milliseconds tick{10};  ///< resolution: a timer fires at its deadline rounded up to a tick
    std::size_t slots = 256;             ///< wheel size; one revolution = tick * slots
    std::size_t max_timers = 1U << 20;   ///< hard bound on live timers (schedule() fails beyond it)
};

/// A hashed timer wheel: a timer with deadline tick T is stored in slot T % slots, so scheduling
/// and cancelling are O(1) and time advances by looking only at the slots passed over. A timer
/// whose deadline is more than one revolution away simply stays in its slot until the visit whose
/// tick has reached it ("rounds"). It is pure: it never reads a clock or touches the OS; the event
/// loop feeds it the time (TESTING_STRATEGY: time is injected).
///
/// Precision: a timer never fires early; it fires on the first advance() whose time has reached
/// the deadline rounded UP to a tick boundary (so at most one tick late).
/// Thread-affinity: loop-thread-only (not synchronised).
class TimerWheel {
public:
    /// `now` is the wheel's epoch. Preconditions (asserted): tick >= 1 ms, slots >= 1.
    TimerWheel(TimerWheelConfig config, TimePoint now);

    /// @return the new timer's id, Error{BufferFull} at max_timers, or Error{ResourceExhausted}
    /// if memory ran out (nothing is scheduled then). A deadline that is already due (before the
    /// time of the last advance) fires at the next advance().
    [[nodiscard]] Result<TimerId> schedule(TimePoint deadline);

    /// @return true if the timer was pending and is now gone; false if it already fired, was
    /// cancelled, or never existed. Always safe to call.
    bool cancel(TimerId id);

    /// Append to `expired` the ids of timers due at `now`, earliest deadline first, ties in
    /// creation order, at most `max` of them; those are removed. If more are due, the rest stay
    /// due for the next call (the caller's per-iteration budget, EC-N15). `now` never moves the
    /// wheel backwards.
    void advance(TimePoint now, std::vector<TimerId>& expired, std::size_t max = SIZE_MAX);

    /// A time at or before the earliest pending deadline (the loop sleeps at most until then);
    /// waking early is harmless. std::nullopt if nothing is pending.
    [[nodiscard]] std::optional<TimePoint> next_wake() const;

    [[nodiscard]] std::size_t size() const noexcept {
        return index_.size();
    }

private:
    struct Entry {
        TimerId id;
        std::int64_t expiry;  // deadline tick, rounded up
    };
    struct Where {
        std::size_t list;
        std::list<Entry>::iterator entry;
    };

    [[nodiscard]] std::int64_t tick_at(TimePoint t) const;     // rounded down
    [[nodiscard]] std::int64_t tick_for_deadline(TimePoint t) const;  // rounded up
    [[nodiscard]] TimePoint time_of(std::int64_t tick) const;

    TimerWheelConfig config_;
    TimePoint start_;
    std::int64_t cursor_ = 0;  // every tick up to here has been fully processed
    TimerId next_id_ = 1;
    std::vector<std::list<Entry>> lists_;  // [0, slots): the wheel; [slots]: already due
    std::unordered_map<TimerId, Where> index_;

    struct Candidate {
        std::int64_t expiry;
        TimerId id;
    };
    std::vector<Candidate> due_;  // scratch for advance(), reused
};

}  // namespace wsrv
