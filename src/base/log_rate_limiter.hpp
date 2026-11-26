// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wsrv {

/// A token bucket per event name (ADR-0037 section 5): a burst of kBurst lines, then one per
/// second. Lines held back are counted, and the count is handed to the next line of that event that
/// gets out, so an operator sees "suppressed=412" instead of a silent gap. The table of names is a
/// fixed array: names beyond kMaxEvents share one bucket, so memory is a constant whatever the
/// code, or a peer, does. Not thread-safe: the caller serialises (the logger holds a mutex around
/// it).
class LogRateLimiter {
public:
    static constexpr std::size_t kMaxEvents = 64;
    static constexpr std::uint64_t kBurst = 10;

    struct Decision {
        bool emit = false;
        /// How many lines of this event were held back since the last one written; 0 unless `emit`.
        std::uint64_t suppressed = 0;
    };

    [[nodiscard]] Decision admit(std::string_view event, TimePoint now) noexcept;

private:
    struct Slot {
        std::uint64_t hash = 0;
        bool used = false;
        bool started = false;
        std::uint64_t millitokens = 0;  // 1000 = one line
        TimePoint last;
        std::uint64_t suppressed = 0;
    };

    [[nodiscard]] Slot& slot_for(std::uint64_t hash) noexcept;

    std::array<Slot, kMaxEvents + 1> slots_{};  // the last one is the shared overflow bucket
};

}  // namespace wsrv
