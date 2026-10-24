// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/logger.hpp"
#include "base/random.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wsrv::test {

/// A clock that only moves when the test says so: timeout logic is tested instantly and repeatably.
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
        std::string message;
    };

    void log(LogLevel level, std::string_view message) override {
        entries.push_back({level, std::string{message}});
    }

    std::vector<Entry> entries;
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
