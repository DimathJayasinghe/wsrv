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
    : config_{config}, start_{now}, lists_(config.slots + 1) {
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
    const bool already_due = expiry <= cursor_;
    const std::size_t list = already_due ? config_.slots
                                         : static_cast<std::size_t>(expiry) % config_.slots;
    const TimerId id = next_id_;
    try {
        lists_[list].push_back(Entry{id, expiry});
        try {
            index_.emplace(id, Where{list, std::prev(lists_[list].end())});
        } catch (const std::bad_alloc&) {
            lists_[list].pop_back();
            throw;
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
    lists_[found->second.list].erase(found->second.entry);
    index_.erase(found);
    return true;
}

void TimerWheel::advance(TimePoint now, std::vector<TimerId>& expired, std::size_t max) {
    const std::int64_t target = std::max(tick_at(now), cursor_);
    due_.clear();
    for (const Entry& entry : lists_[config_.slots]) {  // already due when they were scheduled
        due_.push_back({entry.expiry, entry.id});
    }
    // Visit the slots of the ticks passed over: at most one full revolution, since N consecutive
    // ticks cover every slot once and any entry whose tick has been reached is collected.
    const std::int64_t visits = std::min<std::int64_t>(target - cursor_, static_cast<std::int64_t>(config_.slots));
    for (std::int64_t i = 1; i <= visits; ++i) {
        const auto slot = static_cast<std::size_t>((cursor_ + i) % static_cast<std::int64_t>(config_.slots));
        for (const Entry& entry : lists_[slot]) {
            if (entry.expiry <= target) {
                due_.push_back({entry.expiry, entry.id});
            }
        }
    }
    std::sort(due_.begin(), due_.end(), [](const Candidate& a, const Candidate& b) {
        return a.expiry != b.expiry ? a.expiry < b.expiry : a.id < b.id;
    });
    const std::size_t take = std::min(due_.size(), max);
    for (std::size_t i = 0; i < take; ++i) {
        expired.push_back(due_[i].id);
        const bool removed = cancel(due_[i].id);
        WSRV_ASSERT(removed);
    }
    if (take == due_.size()) {
        cursor_ = target;  // everything up to here is done; otherwise rescan next time
    }
}

std::optional<TimePoint> TimerWheel::next_wake() const {
    if (index_.empty()) {
        return std::nullopt;
    }
    if (!lists_[config_.slots].empty()) {
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
