// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

namespace wsrv {

/// Monotonic time. Deadlines and timeouts MUST use this, never the wall clock (EC-N16: clock
/// jumps).
using TimePoint = std::chrono::steady_clock::time_point;

/// Source of time, injected into everything that has timeouts so tests can control it.
/// Thread-affinity: implementations are any-thread.
class IClock {
public:
    IClock() = default;
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;
    virtual ~IClock() = default;

    /// @return the current monotonic time. Never decreases between calls on the same clock.
    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

/// The real clock.
class SteadyClock final : public IClock {
public:
    [[nodiscard]] TimePoint now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

}  // namespace wsrv
