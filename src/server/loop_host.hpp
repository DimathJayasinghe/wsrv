// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/logger.hpp"
#include "net/event_loop.hpp"
#include "server/connection_host.hpp"

namespace wsrv {

/// Who is told when a connection has finished (implemented by the ConnectionTable).
class IRetireSink {
public:
    IRetireSink() = default;
    IRetireSink(const IRetireSink&) = delete;
    IRetireSink& operator=(const IRetireSink&) = delete;
    IRetireSink(IRetireSink&&) = delete;
    IRetireSink& operator=(IRetireSink&&) = delete;
    virtual ~IRetireSink() = default;

    virtual void retire(ConnectionId id) = 0;
};

/// The production IConnectionHost: forwards to the EventLoop, the clock and the logger (Adapter).
/// Log lines get the connection id prefixed; the message itself is always a fixed string, so no
/// peer text reaches the log (SECURITY_DESIGN T14).
/// Thread-affinity: loop-thread-only.
class LoopHost final : public IConnectionHost {
public:
    LoopHost(EventLoop& loop, const IClock& clock, ILogger& logger, IRetireSink& sink) noexcept
        : loop_{loop}, clock_{clock}, logger_{logger}, sink_{sink} {}

    [[nodiscard]] Result<WatchHandle>
    watch(int fd, PollEvents interest, IEventHandler& handler) override {
        return loop_.watch(fd, interest, handler);
    }
    [[nodiscard]] Result<void> rewatch(WatchHandle handle, PollEvents interest) override {
        return loop_.rewatch(handle, interest);
    }
    void unwatch(WatchHandle handle) override {
        loop_.unwatch(handle);
    }
    [[nodiscard]] Result<TimerHandle> run_after(std::chrono::milliseconds delay,
                                                Task task) override {
        return loop_.run_after(delay, std::move(task));
    }
    bool cancel(TimerHandle timer) override {
        return loop_.cancel(timer);
    }
    [[nodiscard]] TimePoint now() const noexcept override {
        return clock_.now();
    }
    void log(LogLevel level, ConnectionId id, std::string_view message) override;
    void retire(ConnectionId id) override {
        sink_.retire(id);
    }

private:
    EventLoop& loop_;
    const IClock& clock_;
    ILogger& logger_;
    IRetireSink& sink_;
};

}  // namespace wsrv
