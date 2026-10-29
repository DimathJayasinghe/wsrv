// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/error.hpp"
#include "net/fd.hpp"
#include "net/poller.hpp"
#include "net/syscalls.hpp"
#include "net/timer_wheel.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wsrv {

struct EventLoopConfig {
    /// Fairness budgets: what one iteration may process before looking at everything else again
    /// (EC-N07, EC-N08, EC-N15). The remainder is carried to the next iteration.
    std::size_t max_events_per_iteration = 256;
    std::size_t max_tasks_per_iteration = 256;
    std::size_t max_timers_per_iteration = 256;
    /// Hard bounds (each has a defined overflow result: BufferFull).
    std::size_t max_queued_tasks = 4096;
    std::size_t max_deferred = 65536;
    std::size_t max_watches = 65536;
    std::size_t max_timers = 1U << 20;
};

/// Work handed to a loop. It must not block and should not throw: a std::exception escaping a task
/// is caught at the loop boundary and counted (task_failures()), never propagated.
using Task = std::function<void()>;

/// Receives readiness for a watched descriptor. Edge-triggered: on Read, read until WouldBlock; on
/// Write, write until WouldBlock or the queue is empty. Called on the loop thread; must not throw.
class IEventHandler {
public:
    IEventHandler() = default;
    IEventHandler(const IEventHandler&) = delete;
    IEventHandler& operator=(const IEventHandler&) = delete;
    IEventHandler(IEventHandler&&) = delete;
    IEventHandler& operator=(IEventHandler&&) = delete;
    virtual ~IEventHandler() = default;
    virtual void on_event(PollEvents events) noexcept = 0;
};

/// Names a registration. It carries a generation, never the fd number: when a descriptor is closed
/// and its number reused, an old handle (and any not yet delivered event for it) cannot reach the
/// new owner (EC-N12).
struct WatchHandle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

struct TimerHandle {
    TimerId id = 0;  ///< 0 is never a valid timer
};

/// A single-threaded reactor (ADR-0003): one thread runs run() / run_once() and everything else
/// happens in callbacks on that thread, so connection code needs no locks.
///
///  - Descriptors: watch() registers a descriptor with a handler; readiness arrives as on_event().
///  - Timers: run_after() schedules a Task on a hashed timer wheel (10 ms resolution).
///  - Tasks: post() queues a Task from ANY thread; the loop is woken through an eventfd.
///
/// Thread-safe surface: post(), stop(), task_failures(). Everything else must be called on the loop
/// thread (asserted); before the loop starts, that is the thread that created it. A loop that has
/// been stopped is finished (stop is sticky).
///
/// Per iteration: at most max_events_per_iteration descriptor events, then due timers (at most
/// max_timers_per_iteration), then posted tasks (at most max_tasks_per_iteration); whatever is over
/// the budget runs next iteration, so no source of work can starve the others.
class EventLoop {
public:
    [[nodiscard]] static Result<std::unique_ptr<EventLoop>>
    create(ISyscalls& sys, const IClock& clock, EventLoopConfig config = {});

    /// Run iterations until stop(). @return the error that made the poller unusable, if any.
    [[nodiscard]] Result<void> run();
    /// One iteration, waiting at most `max_wait` for something to do (0 = just look).
    [[nodiscard]] Result<void> run_once(std::chrono::milliseconds max_wait);

    /// Ask run() to return. Any thread; idempotent; wakes a blocked loop.
    void stop() noexcept;

    /// Queue `task` to run on the loop thread. Any thread.
    /// @return Error{BufferFull} when max_queued_tasks are already waiting, Error{InvalidArgument}
    /// for an empty task. Tasks posted by a running task run in the next iteration.
    [[nodiscard]] Result<void> post(Task task);

    /// Queue `task` for the end of the current iteration: after its events, timers and posted
    /// tasks, before the next poll. This is the safe point for work that must not run inside the
    /// callback that asked for it, such as destroying the object whose on_event() is on the stack
    /// (ADR-0013). Loop thread only, so no lock. A task deferred from a deferred task waits for the
    /// next iteration (which does not sleep). Unlike post() it is not a cross-thread channel, so
    /// callers may rely on it for teardown; it still has a bound.
    /// @return Error{BufferFull} when max_deferred are already waiting, Error{InvalidArgument} for
    /// an empty task.
    [[nodiscard]] Result<void> defer(Task task);

    /// Run `task` once, `delay` from now (by the injected clock). Error{BufferFull} at max_timers.
    [[nodiscard]] Result<TimerHandle> run_after(std::chrono::milliseconds delay, Task task);
    /// @return true if the timer was still pending. Safe on fired, cancelled and unknown timers.
    bool cancel(TimerHandle timer);

    /// Watch `fd` for `interest`. The handler must outlive the registration. Error{BufferFull} at
    /// max_watches; errors from the poller (invalid descriptor, already registered) are returned.
    [[nodiscard]] Result<WatchHandle> watch(int fd, PollEvents interest, IEventHandler& handler);
    /// Change the interest (and re-arm the edge). Error{InvalidArgument} for a dead handle.
    [[nodiscard]] Result<void> rewatch(WatchHandle handle, PollEvents interest);
    /// Stop watching. Events already harvested for this handle in the current iteration are
    /// dropped. Safe on a dead handle and from inside a callback.
    void unwatch(WatchHandle handle);

    [[nodiscard]] bool in_loop_thread() const noexcept;
    [[nodiscard]] std::uint64_t task_failures() const noexcept {
        return task_failures_.load();
    }

    /// Construction goes through create(); the passkey keeps this constructor unusable elsewhere
    /// while still letting std::make_unique call it (no raw `new`).
    class Key {
        friend class EventLoop;
        Key() = default;
    };
    EventLoop(Key /*unused*/,
              ISyscalls& sys,
              const IClock& clock,
              EventLoopConfig config,
              std::unique_ptr<IPoller> poller,
              Fd wake_fd);

private:
    struct Watch {
        int fd = -1;
        IEventHandler* handler = nullptr;
        std::uint32_t generation = 1;
        bool active = false;
    };

    [[nodiscard]] static std::uint64_t token_of(std::uint32_t index,
                                                std::uint32_t generation) noexcept;
    [[nodiscard]] Watch* find(WatchHandle handle) noexcept;
    void request_wake() noexcept;
    void write_wake() noexcept;
    void drain_wake() noexcept;
    void dispatch(const PollEvent& event);
    void run_timers();
    void run_tasks();
    void run_deferred();
    void invoke(Task& task) noexcept;
    [[nodiscard]] int wait_timeout_ms(std::chrono::milliseconds max_wait);

    ISyscalls& sys_;
    const IClock& clock_;
    EventLoopConfig config_;
    std::unique_ptr<IPoller> poller_;
    Fd wake_fd_;
    TimerWheel wheel_;
    std::unordered_map<TimerId, Task> timer_tasks_;
    std::vector<Watch> watches_;
    std::vector<std::uint32_t> free_watches_;
    std::vector<PollEvent> events_;
    std::vector<TimerId> expired_;

    std::mutex tasks_mutex_;
    std::deque<Task> tasks_;  // guarded by tasks_mutex_
    /// A wake-up is already guaranteed: some producer has written (or is about to write) the
    /// eventfd, or the loop will pop the queue before it next sleeps. Set by producers and cleared
    /// by the loop, both ONLY while holding tasks_mutex_ - see request_wake().
    bool wake_requested_ = false;  // guarded by tasks_mutex_
    std::vector<Task> batch_;      // loop thread only
    std::vector<Task> deferred_;   // loop thread only: waiting for the end of the iteration
    std::vector<Task> sweeping_;   // loop thread only: the deferred tasks being run now
    std::atomic<bool> stopping_{false};
    std::atomic<std::thread::id> owner_;
    std::atomic<std::uint64_t> task_failures_{0};
};

}  // namespace wsrv
