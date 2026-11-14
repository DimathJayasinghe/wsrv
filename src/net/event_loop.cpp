// SPDX-License-Identifier: Apache-2.0
#include "net/event_loop.hpp"

#include "base/assert.hpp"
#include "net/epoll_poller.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <new>

#include <sys/eventfd.h>

namespace wsrv {

namespace {

/// The wake-up eventfd's token. Watch tokens are never 0 (see token_of).
constexpr std::uint64_t kWakeToken = 0;

/// run() sleeps at most this long when there is nothing to wait for; stop() and post() interrupt
/// it anyway, this is only a safety net.
constexpr std::chrono::milliseconds kIdleWait{60000};

}  // namespace

EventLoop::EventLoop(Key /*unused*/,
                     ISyscalls& sys,
                     const IClock& clock,
                     EventLoopConfig config,
                     std::unique_ptr<IPoller> poller,
                     Fd wake_fd)
    : sys_{sys}, clock_{clock}, config_{config}, poller_{std::move(poller)},
      wake_fd_{std::move(wake_fd)},
      wheel_{TimerWheelConfig{std::chrono::milliseconds{10}, 256, config.max_timers}, clock.now()},
      events_(std::max<std::size_t>(config.max_events_per_iteration, 1)),
      owner_{std::this_thread::get_id()} {}

Result<std::unique_ptr<EventLoop>>
EventLoop::create(ISyscalls& sys, const IClock& clock, EventLoopConfig config) {
    auto poller = EpollPoller::create(sys);
    if (!poller) {
        return std::unexpected{poller.error()};
    }
    const Result<long> wake =
        retrying([&] { return sys.eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }, "eventfd");
    if (!wake) {
        return std::unexpected{wake.error()};
    }
    Fd wake_fd{static_cast<int>(*wake)};
    if (const Result<void> added = (*poller)->add(wake_fd.get(), PollEvents::Read, kWakeToken);
        !added) {
        return std::unexpected{added.error()};
    }
    try {
        return std::make_unique<EventLoop>(
            Key{}, sys, clock, config, std::move(*poller), std::move(wake_fd));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "event loop: out of memory");
    }
}

bool EventLoop::in_loop_thread() const noexcept {
    return owner_.load() == std::this_thread::get_id();
}

std::uint64_t EventLoop::token_of(std::uint32_t index, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32U) |
           (static_cast<std::uint64_t>(index) + 1U);
}

EventLoop::Watch* EventLoop::find(WatchHandle handle) noexcept {
    if (handle.index >= watches_.size()) {
        return nullptr;
    }
    Watch& watch = watches_[handle.index];
    return watch.active && watch.generation == handle.generation ? &watch : nullptr;
}

// ---- cross-thread surface --------------------------------------------------------------------

/// Write to the eventfd so a blocked epoll_wait returns.
void EventLoop::write_wake() noexcept {
    const std::uint64_t one = 1;
    for (std::size_t attempt = 0; attempt < kMaxEintrRetries; ++attempt) {
        if (sys_.write(wake_fd_.get(), &one, sizeof(one)).err != EINTR) {
            return;  // EAGAIN means the counter is full: a wake-up is pending anyway
        }
    }
}

/// Make sure the loop runs another iteration soon. Many callers collapse into one eventfd write
/// through `wake_requested_`, which is read and written only under tasks_mutex_, the same lock that
/// guards the queue. That is what makes it correct: the loop clears the flag in the very critical
/// section in which it pops the queue (run_tasks), so
///   - a producer that sees the flag set pushed (or will push) before that pop, and the pop sees
///   it;
///   - a producer that sees it clear writes the eventfd after pushing, which wakes the next wait.
/// (An earlier version used an atomic flag cleared outside the lock; a producer could set it
/// between the loop's clear and its eventfd read, leaving "flag set, eventfd drained", and every
/// later post skipped the write: the sleeping loop was never woken.)
void EventLoop::request_wake() noexcept {
    bool write = false;
    {
        const std::lock_guard<std::mutex> lock{tasks_mutex_};
        write = !wake_requested_;
        wake_requested_ = true;
    }
    if (write) {
        write_wake();
    }
}

void EventLoop::drain_wake() noexcept {
    std::uint64_t counter = 0;
    for (std::size_t attempt = 0; attempt < kMaxEintrRetries; ++attempt) {
        if (sys_.read(wake_fd_.get(), &counter, sizeof(counter)).err != EINTR) {
            return;
        }
    }
}

void EventLoop::stop() noexcept {
    stopping_.store(true);
    request_wake();
}

Result<void> EventLoop::post(Task task) {
    return post_within(std::move(task), config_.max_queued_tasks);
}

Result<void> EventLoop::post_bulk(Task task) {
    // A quarter of the queue (at least one slot when the queue has four or more) is kept back.
    return post_within(std::move(task), config_.max_queued_tasks - config_.max_queued_tasks / 4);
}

Result<void> EventLoop::post_within(Task task, std::size_t limit) {
    if (!task) {
        return fail(ErrorCode::InvalidArgument, "empty task");
    }
    bool write = false;
    {
        const std::lock_guard<std::mutex> lock{tasks_mutex_};
        if (tasks_.size() >= limit) {
            return fail(ErrorCode::BufferFull, "event loop task queue is full");
        }
        try {
            tasks_.push_back(std::move(task));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceExhausted, "event loop: out of memory");
        }
        write = !wake_requested_;  // decided in the same critical section as the push
        wake_requested_ = true;
    }
    if (write) {
        write_wake();
    }
    return {};
}

// ---- loop-thread API -------------------------------------------------------------------------

Result<void> EventLoop::defer(Task task) {
    WSRV_ASSERT(in_loop_thread());
    if (!task) {
        return fail(ErrorCode::InvalidArgument, "empty task");
    }
    if (deferred_.size() >= config_.max_deferred) {
        return fail(ErrorCode::BufferFull, "event loop deferred queue is full");
    }
    try {
        deferred_.push_back(std::move(task));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "event loop: out of memory");
    }
    return {};
}

Result<TimerHandle> EventLoop::run_after(std::chrono::milliseconds delay, Task task) {
    WSRV_ASSERT(in_loop_thread());
    if (!task) {
        return fail(ErrorCode::InvalidArgument, "empty task");
    }
    const Result<TimerId> id =
        wheel_.schedule(clock_.now() + std::max(delay, std::chrono::milliseconds{0}));
    if (!id) {
        return std::unexpected{id.error()};
    }
    try {
        timer_tasks_.emplace(*id, std::move(task));
    } catch (const std::bad_alloc&) {
        wheel_.cancel(*id);
        return fail(ErrorCode::ResourceExhausted, "event loop: out of memory");
    }
    return TimerHandle{*id};
}

bool EventLoop::cancel(TimerHandle timer) {
    WSRV_ASSERT(in_loop_thread());
    const bool pending = wheel_.cancel(timer.id);
    timer_tasks_.erase(timer.id);
    return pending;
}

Result<WatchHandle> EventLoop::watch(int fd, PollEvents interest, IEventHandler& handler) {
    WSRV_ASSERT(in_loop_thread());
    std::uint32_t index = 0;
    if (!free_watches_.empty()) {
        index = free_watches_.back();
        free_watches_.pop_back();
    } else if (watches_.size() < config_.max_watches) {
        try {
            watches_.emplace_back();
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceExhausted, "event loop: out of memory");
        }
        index = static_cast<std::uint32_t>(watches_.size() - 1);
    } else {
        return fail(ErrorCode::BufferFull, "event loop watch table is full");
    }
    Watch& watch = watches_[index];
    if (const Result<void> added = poller_->add(fd, interest, token_of(index, watch.generation));
        !added) {
        free_watches_.push_back(index);  // the slot was never used
        return std::unexpected{added.error()};
    }
    watch.fd = fd;
    watch.handler = &handler;
    watch.active = true;
    return WatchHandle{index, watch.generation};
}

Result<void> EventLoop::rewatch(WatchHandle handle, PollEvents interest) {
    WSRV_ASSERT(in_loop_thread());
    const Watch* watch = find(handle);
    if (watch == nullptr) {
        return fail(ErrorCode::InvalidArgument, "dead watch handle");
    }
    return poller_->modify(watch->fd, interest, token_of(handle.index, handle.generation));
}

void EventLoop::unwatch(WatchHandle handle) {
    WSRV_ASSERT(in_loop_thread());
    Watch* watch = find(handle);
    if (watch == nullptr) {
        return;
    }
    // Ignored on purpose: if the descriptor was already closed the kernel dropped the registration.
    static_cast<void>(poller_->remove(watch->fd).has_value());
    watch->active = false;
    watch->handler = nullptr;
    ++watch->generation;  // every event still carrying the old token is now stale
    if (watch->generation == 0) {
        watch->generation = 1;  // 0 is skipped so a token never equals kWakeToken
    }
    free_watches_.push_back(handle.index);
}

// ---- the iteration ---------------------------------------------------------------------------

void EventLoop::invoke(Task& task) noexcept {
    try {
        task();
    } catch (const std::exception&) {
        task_failures_.fetch_add(1);
    }
}

int EventLoop::wait_timeout_ms(std::chrono::milliseconds max_wait) {
    if (!deferred_.empty()) {
        return 0;  // work deferred during the last sweep: do not sleep
    }
    {
        const std::lock_guard<std::mutex> lock{tasks_mutex_};
        if (!tasks_.empty()) {
            return 0;  // carried-over tasks: do not sleep
        }
    }
    std::chrono::milliseconds wait = std::max(max_wait, std::chrono::milliseconds{0});
    if (const std::optional<TimePoint> due = wheel_.next_wake()) {
        const TimePoint now = clock_.now();
        const auto until = *due > now ? std::chrono::ceil<std::chrono::milliseconds>(*due - now)
                                      : std::chrono::milliseconds{0};
        wait = std::min(wait, until);
    }
    return static_cast<int>(std::min<std::int64_t>(wait.count(), INT_MAX));
}

void EventLoop::dispatch(const PollEvent& event) {
    if (event.token == kWakeToken) {
        drain_wake();
        return;
    }
    const auto index = static_cast<std::uint32_t>((event.token & 0xFFFFFFFFU) - 1U);
    const auto generation = static_cast<std::uint32_t>(event.token >> 32U);
    Watch* watch = find(WatchHandle{index, generation});
    if (watch == nullptr) {
        return;  // unwatched (or replaced) since the kernel reported it: drop it (EC-N12)
    }
    IEventHandler* handler = watch->handler;  // copy: the callback may change the table
    handler->on_event(event.events);
}

void EventLoop::run_timers() {
    expired_.clear();
    wheel_.advance(clock_.now(), expired_, config_.max_timers_per_iteration);
    for (const TimerId id : expired_) {
        const auto found = timer_tasks_.find(id);
        if (found == timer_tasks_.end()) {
            continue;  // cancelled by an earlier timer in this very batch
        }
        Task task = std::move(found->second);
        timer_tasks_.erase(found);
        invoke(task);
    }
}

void EventLoop::run_tasks() {
    batch_.clear();
    {
        const std::lock_guard<std::mutex> lock{tasks_mutex_};
        wake_requested_ = false;  // from here on a new post must write the eventfd again
        while (!tasks_.empty() && batch_.size() < config_.max_tasks_per_iteration) {
            batch_.push_back(std::move(tasks_.front()));
            tasks_.pop_front();
        }
    }
    for (Task& task : batch_) {
        invoke(task);
    }
    batch_.clear();
}

/// Run what was deferred before this sweep began. Swapping first is what makes a task deferred
/// from inside the sweep wait for the next iteration instead of extending this one forever.
void EventLoop::run_deferred() {
    sweeping_.clear();
    sweeping_.swap(deferred_);
    for (Task& task : sweeping_) {
        invoke(task);
    }
    sweeping_.clear();
}

Result<void> EventLoop::run_once(std::chrono::milliseconds max_wait) {
    owner_.store(std::this_thread::get_id());
    const Result<std::size_t> ready = poller_->wait(wait_timeout_ms(max_wait), events_);
    if (!ready) {
        return std::unexpected{ready.error()};
    }
    for (std::size_t i = 0; i < *ready; ++i) {
        dispatch(events_[i]);
    }
    run_timers();
    run_tasks();
    run_deferred();
    return {};
}

Result<void> EventLoop::run() {
    while (!stopping_.load()) {
        if (const Result<void> step = run_once(kIdleWait); !step) {
            return step;
        }
    }
    return {};
}

}  // namespace wsrv
