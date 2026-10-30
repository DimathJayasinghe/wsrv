// SPDX-License-Identifier: Apache-2.0
#include "capi/event_queue.hpp"

#include "base/assert.hpp"

#include <new>
#include <utility>

#include <sys/eventfd.h>

namespace wsrv {

namespace {

/// The level at which a pressure episode starts: three quarters of `limit`.
constexpr std::size_t high_water(std::size_t limit) noexcept {
    return limit - limit / 4;
}

}  // namespace

EventQueue::EventQueue(Key /*unused*/, ISyscalls& sys, EventQueueConfig config, Fd wake) noexcept
    : sys_{sys}, config_{config}, high_events_{high_water(config.max_events)},
      high_bytes_{high_water(config.max_bytes)}, low_events_{config.max_events / 2},
      low_bytes_{config.max_bytes / 2}, wake_{std::move(wake)} {}

Result<std::unique_ptr<EventQueue>> EventQueue::create(ISyscalls& sys, EventQueueConfig config) {
    if (config.max_events < 2 || config.max_bytes < 2 * kReservedCloseBytes) {
        return fail(ErrorCode::InvalidConfig, "event queue: limits too small for one connection");
    }
    const Result<long> wake =
        retrying([&] { return sys.eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }, "eventfd");
    if (!wake) {
        return std::unexpected{wake.error()};
    }
    try {
        return std::make_unique<EventQueue>(Key{}, sys, config, Fd{static_cast<int>(*wake)});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "event queue: out of memory");
    }
}

// Both counters never exceed their limit (the invariant every push preserves), so the
// subtractions below cannot wrap.
bool EventQueue::fits(std::size_t events, std::size_t bytes) const noexcept {
    const std::size_t events_level = events_.size() + reserved_closes_;
    const std::size_t bytes_level = bytes_ + reserved_bytes_;
    WSRV_ASSERT(events_level <= config_.max_events);
    WSRV_ASSERT(bytes_level <= config_.max_bytes);
    return events <= config_.max_events - events_level && bytes <= config_.max_bytes - bytes_level;
}

bool EventQueue::at_high_water() const noexcept {
    return events_.size() + reserved_closes_ >= high_events_ ||
           bytes_ + reserved_bytes_ >= high_bytes_;
}

bool EventQueue::at_low_water() const noexcept {
    return events_.size() + reserved_closes_ <= low_events_ &&
           bytes_ + reserved_bytes_ <= low_bytes_;
}

/// Decide whether `event` may enter. Open needs room for itself and its Close; Close needs the
/// reservation an Open made; everything else needs room for itself.
Result<void> EventQueue::admit(const HostEvent& event) {
    const std::size_t size = event.data.size();
    if (event.type == HostEventType::Close) {
        if (reserved_closes_ == 0 || size > kReservedCloseBytes) {
            return fail(ErrorCode::InvalidArgument, "event queue: Close without a reservation");
        }
        return {};
    }
    const bool is_open = event.type == HostEventType::Open;
    if (!fits(is_open ? 2 : 1, is_open ? size + kReservedCloseBytes : size)) {
        dropped_.fetch_add(1);
        return fail(ErrorCode::BufferFull, "event queue: full");
    }
    return {};
}

Result<PushOutcome> EventQueue::push(HostEvent event) {
    const std::lock_guard lock{mutex_};
    if (const Result<void> admitted = admit(event); !admitted) {
        return std::unexpected{admitted.error()};
    }
    const std::size_t size = event.data.size();
    const HostEventType type = event.type;
    const bool was_empty = events_.empty();
    try {
        events_.push_back(std::move(event));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "event queue: out of memory");
    }
    if (was_empty) {
        const std::uint64_t one = 1;
        const Result<long> woke = retrying(
            [&] { return sys_.write(wake_.get(), &one, sizeof(one)); }, "event queue wake-up");
        if (!woke) {
            events_.pop_back();  // no wake-up could be arranged: do not hold an event nobody sees
            return std::unexpected{woke.error()};
        }
    }
    bytes_ += size;
    if (type == HostEventType::Open) {
        ++reserved_closes_;
        reserved_bytes_ += kReservedCloseBytes;
    } else if (type == HostEventType::Close) {
        --reserved_closes_;
        reserved_bytes_ -= kReservedCloseBytes;
    }
    if (at_high_water()) {
        pressured_ = true;
    }
    return PushOutcome{pressured_};
}

bool EventQueue::pressured() const {
    const std::lock_guard lock{mutex_};
    return pressured_;
}

std::size_t EventQueue::pending() const {
    const std::lock_guard lock{mutex_};
    return events_.size();
}

std::optional<Popped> EventQueue::try_pop() {
    const std::lock_guard lock{mutex_};
    if (events_.empty()) {
        return std::nullopt;
    }
    Popped popped{std::move(events_.front()), false};
    events_.pop_front();
    bytes_ -= popped.event.data.size();
    if (events_.empty()) {
        std::uint64_t counter = 0;
        // Clearing the counter makes the descriptor not readable. If this somehow fails the
        // descriptor stays readable and the host wakes up for nothing: harmless.
        static_cast<void>(sys_.read(wake_.get(), &counter, sizeof(counter)));
    }
    if (pressured_ && at_low_water()) {
        pressured_ = false;
        popped.resume_reads = true;
    }
    return popped;
}

}  // namespace wsrv
