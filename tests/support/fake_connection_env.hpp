// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/transport.hpp"
#include "server/connection_host.hpp"
#include "support/fakes.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace wsrv::test {

/// A transport driven by the test: queued input (bytes, EOF or an error), a write capacity per
/// call, and a record of everything written. No kernel involved.
class FakeTransport final : public ITransport {
public:
    /// Bytes the "peer" sends. They are delivered in whatever size the reader asks for.
    void feed(ByteView bytes) {
        incoming_.insert(incoming_.end(), bytes.begin(), bytes.end());
    }
    void feed(std::string_view text) {
        feed(as_bytes(text));
    }
    void feed_eof() {
        eof_ = true;
    }
    void fail_reads(ErrorCode code) {
        read_error_ = code;
    }
    void fail_writes(ErrorCode code) {
        write_error_ = code;
    }
    /// The kernel buffer has room for `n` more bytes; 0 = full (WouldBlock) until the test makes
    /// room by calling this again.
    void set_write_capacity(std::size_t n) {
        write_capacity_ = n;
    }

    [[nodiscard]] Result<std::size_t> read(MutableByteView out) override {
        ++read_calls;
        if (read_error_) {
            return fail(*read_error_, "fake read");
        }
        if (incoming_.empty()) {
            return eof_ ? Result<std::size_t>{0} : fail(ErrorCode::WouldBlock, "fake read");
        }
        const std::size_t n = std::min(out.size(), incoming_.size());
        std::copy_n(incoming_.begin(), n, out.begin());
        incoming_.erase(incoming_.begin(), incoming_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    [[nodiscard]] Result<std::size_t> write(ByteView data) override {
        ++write_calls;
        if (write_error_) {
            return fail(*write_error_, "fake write");
        }
        if (write_capacity_ == 0) {
            return fail(ErrorCode::WouldBlock, "fake write");
        }
        const std::size_t n = std::min(data.size(), write_capacity_);
        if (write_capacity_ != kUnlimited) {
            write_capacity_ -= n;
        }
        written.insert(written.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    void shutdown_write() noexcept override {
        shut_down = true;
    }
    void close() noexcept override {
        closed = true;
    }
    [[nodiscard]] int fd() const noexcept override {
        return closed ? -1 : 42;
    }

    [[nodiscard]] std::string written_text() const {
        return std::string{as_chars(ByteView{written})};
    }
    [[nodiscard]] std::size_t unread() const {
        return incoming_.size();
    }

    std::vector<std::byte> written;
    std::size_t read_calls = 0;
    std::size_t write_calls = 0;
    bool shut_down = false;
    bool closed = false;

private:
    std::deque<std::byte> incoming_;
    bool eof_ = false;
    std::optional<ErrorCode> read_error_;
    std::optional<ErrorCode> write_error_;
    static constexpr std::size_t kUnlimited = static_cast<std::size_t>(-1);
    std::size_t write_capacity_ = kUnlimited;
};

/// Manual timers and a recording of what the connection asked of the loop.
class FakeHost final : public IConnectionHost {
public:
    [[nodiscard]] Result<WatchHandle>
    watch(int /*fd*/, PollEvents interest, IEventHandler& /*handler*/) override {
        watching = true;
        interest_now = interest;
        return WatchHandle{1, 1};
    }
    [[nodiscard]] Result<void> rewatch(WatchHandle /*handle*/, PollEvents interest) override {
        ++rewatch_calls;
        interest_now = interest;
        return {};
    }
    void unwatch(WatchHandle /*handle*/) override {
        watching = false;
    }
    [[nodiscard]] Result<TimerHandle> run_after(std::chrono::milliseconds delay,
                                                Task task) override {
        const TimerId id = ++next_id_;
        timers_[id] = Timer{clock.now() + delay, std::move(task)};
        return TimerHandle{id};
    }
    bool cancel(TimerHandle timer) override {
        return timers_.erase(timer.id) > 0;
    }
    [[nodiscard]] TimePoint now() const noexcept override {
        return clock.now();
    }
    void log(LogLevel level, ConnectionId /*id*/, std::string_view message) override {
        logs.push_back({level, std::string{message}});
    }
    void retire(ConnectionId id) override {
        retired.push_back(id);
    }

    /// Move time forward in steps of `step`, firing timers that fall due (a timer may re-arm).
    void advance(std::chrono::milliseconds total, std::chrono::milliseconds step) {
        for (std::chrono::milliseconds done{0}; done < total; done += step) {
            clock.advance(step);
            fire_due();
        }
    }
    [[nodiscard]] std::size_t pending_timers() const {
        return timers_.size();
    }
    [[nodiscard]] bool logged(std::string_view needle) const {
        return std::ranges::any_of(logs, [&](const auto& entry) {
            return entry.message.find(needle) != std::string::npos;
        });
    }

    FakeClock clock;
    bool watching = false;
    PollEvents interest_now = PollEvents::None;
    std::size_t rewatch_calls = 0;
    std::vector<ConnectionId> retired;
    std::vector<RecordingLogger::Entry> logs;

private:
    struct Timer {
        TimePoint due;
        Task task;
    };
    void fire_due() {
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.due <= clock.now()) {
                Task task = std::move(it->second.task);
                it = timers_.erase(it);
                task();
                it = timers_.begin();  // the task may have changed the table
            } else {
                ++it;
            }
        }
    }
    std::map<TimerId, Timer> timers_;
    TimerId next_id_ = 0;
};

}  // namespace wsrv::test
