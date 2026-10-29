// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/event_loop.hpp"
#include "net/fd.hpp"
#include "net/syscalls.hpp"

#include <csignal>
#include <functional>
#include <memory>
#include <span>

namespace wsrv {

/// Turns process signals into ordinary loop events (EC-N17).
///
/// A signal handler runs at an arbitrary instruction, where almost nothing is safe to call. The
/// alternative is to BLOCK the signals and read them from a signalfd: the kernel then queues them
/// like data on a socket, and the loop delivers them to the callback on the loop thread, where
/// shutting down is as safe as any other callback.
///
/// create() blocks `signals` for the calling thread (pthread_sigmask) and the destructor restores
/// the previous mask after discarding anything still pending: unblocking a pending SIGTERM would
/// run its default action and kill the process. Signal masks are per thread, so create the watcher
/// on the thread that will run the loop and before starting any other thread (threads inherit the
/// mask; a thread that does not block the signal could take it first). SIGPIPE needs no handling:
/// every send uses MSG_NOSIGNAL (EC-N03).
///
/// The callback runs on the loop thread, must not throw, and may call EventLoop::stop().
/// Thread-affinity: loop-thread-only. Destroy before the EventLoop it uses.
class SignalWatcher final : public IEventHandler {
public:
    using Callback = std::function<void(int signo)>;

    /// @return Error{InvalidArgument} for an empty list, an invalid signal number or an empty
    /// callback; Error{FdLimit} / Error{Io} from the kernel. On failure the mask is unchanged.
    [[nodiscard]] static Result<std::unique_ptr<SignalWatcher>>
    create(EventLoop& loop, ISyscalls& sys, std::span<const int> signals, Callback callback);

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;
    SignalWatcher(SignalWatcher&&) = delete;
    SignalWatcher& operator=(SignalWatcher&&) = delete;
    ~SignalWatcher() override;

    void on_event(PollEvents events) noexcept override;

    /// Construction goes through create() (the passkey keeps this unusable elsewhere while
    /// std::make_unique can still call it: no raw `new`).
    class Key {
        friend class SignalWatcher;
        Key() = default;
    };
    SignalWatcher(Key /*unused*/,
                  EventLoop& loop,
                  ISyscalls& sys,
                  Callback callback,
                  Fd signal_fd,
                  const sigset_t& previous_mask);

private:
    /// Read queued signals until WouldBlock. `deliver == false` discards them (destructor).
    void drain(bool deliver) noexcept;

    EventLoop& loop_;
    ISyscalls& sys_;
    Callback callback_;
    Fd signal_fd_;
    sigset_t previous_mask_;
    WatchHandle watch_{};
};

}  // namespace wsrv
