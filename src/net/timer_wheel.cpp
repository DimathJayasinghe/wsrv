// SPDX-License-Identifier: Apache-2.0
#include "net/timer_wheel.hpp"

#include "base/assert.hpp"

#include <algorithm>
#include <new>

namespace wsrv {

namespace {

/// A deadline further away than this is clamped (a century), so tick arithmetic cannot overflow
/// even for TimePoint::max().
constexpr std::chrono::nanoseconds kHorizon = std::chrono::hours{24} * 365 * 100;

}  // namespace

TimerWheel::TimerWheel(TimerWheelConfig config, TimePoint now)
    : config_{config}, start_{now}, lists_(config.slots) {
    WSRV_ASSERT(config.tick >= std::chrono::milliseconds{1});
    WSRV_ASSERT(config.slots >= 1);
}

std::int64_t TimerWheel::tick_at(TimePoint t) const {
    if (t <= start_) {
        return 0;
    }
    const auto delta = std::min<std::chrono::nanoseconds>(t - start_, kHorizon);
    return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() /
           config_.tick.count();
}

std::int64_t TimerWheel::tick_for_deadline(TimePoint t) const {
    if (t <= start_) {
        return 0;
    }
    const auto delta = std::min<std::chrono::nanoseconds>(t - start_, kHorizon);
    const std::int64_t ms = std::chrono::ceil<std::chrono::milliseconds>(delta).count();
    return (ms + config_.tick.count() - 1) / config_.tick.count();
}

TimePoint TimerWheel::time_of(std::int64_t tick) const {
    return start_ + config_.tick * tick;
}

Result<TimerId> TimerWheel::schedule(TimePoint deadline) {
    if (index_.size() >= config_.max_timers) {
        return fail(ErrorCode::BufferFull, "timer wheel is full");
    }
    const std::int64_t expiry = tick_for_deadline(deadline);
    const TimerId id = next_id_;
    try {
        if (expiry <= cursor_) {  // already due: straight to the ready set
            ready_.insert({expiry, id});
            try {
                index_.emplace(id, Where{config_.slots, {}, expiry});
            } catch (const std::bad_alloc&) {
                ready_.erase({expiry, id});
                throw;
            }
        } else {
            std::list<Entry>& slot = lists_[static_cast<std::size_t>(expiry) % config_.slots];
            slot.push_back(Entry{id, expiry});
            try {
                index_.emplace(id, Where{static_cast<std::size_t>(expiry) % config_.slots, std::prev(slot.end()), expiry});
            } catch (const std::bad_alloc&) {
                slot.pop_back();
                throw;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "timer wheel: out of memory");
    }
    ++next_id_;
    return id;
}

bool TimerWheel::cancel(TimerId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    const Where& where = found->second;
    if (where.list == config_.slots) {
        ready_.erase({where.expiry, id});
    } else {
        lists_[where.list].erase(where.entry);
    }
    index_.erase(found);
    return true;
}

void TimerWheel::advance(TimePoint now, std::vector<TimerId>& expired, std::size_t max) {
    const std::int64_t target = std::max(tick_at(now), cursor_);
    // Move every entry whose tick has been reached into the ready set. Visiting at most one full
    // revolution of slots suffices: N consecutive ticks cover every slot once.
    const std::int64_t visits =
        std::min<std::int64_t>(target - cursor_, static_cast<std::int64_t>(config_.slots));
    for (std::int64_t i = 1; i <= visits; ++i) {
        std::list<Entry>& slot =
            lists_[static_cast<std::size_t>((cursor_ + i) % static_cast<std::int64_t>(config_.slots))];
        for (auto it = slot.begin(); it != slot.end();) {
            if (it->expiry <= target) {
                ready_.insert({it->expiry, it->id});
                index_.at(it->id) = Where{config_.slots, {}, it->expiry};
                it = slot.erase(it);
            } else {
                ++it;
            }
        }
    }
    cursor_ = target;
    for (std::size_t taken = 0; taken < max && !ready_.empty(); ++taken) {
        const TimerId id = ready_.begin()->second;
        expired.push_back(id);
        ready_.erase(ready_.begin());
        index_.erase(id);
    }
}

std::optional<TimePoint> TimerWheel::next_wake() const {
    if (index_.empty()) {
        return std::nullopt;
    }
    if (!ready_.empty()) {
        return time_of(cursor_);  // something is already due
    }
    for (std::size_t offset = 1; offset <= config_.slots; ++offset) {
        const auto tick = cursor_ + static_cast<std::int64_t>(offset);
        if (!lists_[static_cast<std::size_t>(tick) % config_.slots].empty()) {
            return time_of(tick);  // a lower bound: the entry may belong to a later round
        }
    }
    return time_of(cursor_ + static_cast<std::int64_t>(config_.slots));  // unreachable if index_ is consistent
}

}  // namespace wsrv
