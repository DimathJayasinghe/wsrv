// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/error.hpp"
#include "base/logger.hpp"
#include "net/event_loop.hpp"
#include "server/handler.hpp"

#include <chrono>
#include <string_view>

namespace wsrv {

/// The few things a Connection needs from the loop and the server around it (Interface
/// Segregation: not the whole EventLoop). The real implementation forwards to the EventLoop; tests
/// use a fake with manual timers, so connection behaviour is checked without a kernel.
///
/// Thread-affinity: loop-thread-only.
class IConnectionHost {
public:
    IConnectionHost() = default;
    IConnectionHost(const IConnectionHost&) = delete;
    IConnectionHost& operator=(const IConnectionHost&) = delete;
    IConnectionHost(IConnectionHost&&) = delete;
    IConnectionHost& operator=(IConnectionHost&&) = delete;
    virtual ~IConnectionHost() = default;

    [[nodiscard]] virtual Result<WatchHandle>
    watch(int fd, PollEvents interest, IEventHandler& handler) = 0;
    [[nodiscard]] virtual Result<void> rewatch(WatchHandle handle, PollEvents interest) = 0;
    virtual void unwatch(WatchHandle handle) = 0;

    [[nodiscard]] virtual Result<TimerHandle> run_after(std::chrono::milliseconds delay,
                                                        Task task) = 0;
    virtual bool cancel(TimerHandle timer) = 0;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;

    /// `message` must be a fixed string: peer-supplied text is never passed to the log (T14).
    virtual void log(LogLevel level, ConnectionId id, std::string_view message) = 0;

    /// The connection has finished and released everything it owns; the owner may destroy it at
    /// the next safe point (ADR-0013 decision 3). Called exactly once per connection.
    virtual void retire(ConnectionId id) = 0;
};

}  // namespace wsrv
