// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/logger.hpp"
#include "base/random.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wsrv::test {

/// A clock that only moves when the test says so: timeout logic is tested instantly and repeatably.
/// Calendar time a test sets by hand (a log line's timestamp).
class FakeWallClock final : public IWallClock {
public:
    [[nodiscard]] WallTime now() const noexcept override {
        return now_;
    }
    void set(WallTime time) noexcept {
        now_ = time;
    }

private:
    WallTime now_{};
};

class FakeClock final : public IClock {
public:
    [[nodiscard]] TimePoint now() const noexcept override {
        return now_;
    }
    void advance(std::chrono::nanoseconds amount) noexcept {
        now_ += amount;
    }

private:
    TimePoint now_{};
};

/// Records every log call so tests can assert on what was (not) logged.
class RecordingLogger final : public ILogger {
public:
    struct Entry {
        LogLevel level;
        std::string event;
        std::string message;
        std::uint64_t conn = 0;
        std::string peer;
    };

    using ILogger::log;
    void log(const LogRecord& record) override {
        entries.push_back({record.level,
                           std::string{record.event},
                           std::string{record.message},
                           record.conn,
                           std::string{record.peer}});
    }

    std::vector<Entry> entries;
};

/// A logger several threads may write while the test thread waits for a line (a running server logs
/// from its own threads). `wait_for` polls; no sleeping beyond a few seconds in total.
class SharedLogger final : public ILogger {
public:
    using ILogger::log;
    void log(const LogRecord& record) override {
        const std::lock_guard lock{mutex_};
        entries_.push_back({record.level,
                            std::string{record.event},
                            std::string{record.message},
                            record.conn,
                            std::string{record.peer}});
    }
    /// The first entry named `event`, waiting up to `timeout`; empty optional if it never came.
    [[nodiscard]] std::optional<RecordingLogger::Entry>
    wait_for(std::string_view event, std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            {
                const std::lock_guard lock{mutex_};
                for (const auto& entry : entries_) {
                    if (entry.event == event) {
                        return entry;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    /// Every line so far as "event|level|message|peer", one per line, for "was X ever written".
    [[nodiscard]] std::string everything() const {
        const std::lock_guard lock{mutex_};
        std::string all;
        for (const auto& entry : entries_) {
            all += entry.event + "|" + std::to_string(static_cast<int>(entry.level)) + "|" +
                   entry.message + "|" + entry.peer + "\n";
        }
        return all;
    }

private:
    mutable std::mutex mutex_;
    std::vector<RecordingLogger::Entry> entries_;
};

/// Deterministic "random" bytes (an incrementing counter) with failure injection.
class FakeRandom final : public IRandom {
public:
    explicit FakeRandom(std::uint8_t first = 0) noexcept : next_{first} {}

    void fail_next() noexcept {
        fail_next_ = true;
    }

    [[nodiscard]] Result<void> fill(MutableByteView out) override {
        if (fail_next_) {
            fail_next_ = false;
            return fail(ErrorCode::RandomUnavailable, "injected failure");
        }
        for (std::byte& b : out) {
            b = static_cast<std::byte>(next_++);
        }
        return {};
    }

private:
    std::uint8_t next_;
    bool fail_next_ = false;
};

}  // namespace wsrv::test
