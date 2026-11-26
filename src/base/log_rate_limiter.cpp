// SPDX-License-Identifier: Apache-2.0
#include "base/log_rate_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace wsrv {

namespace {

constexpr std::uint64_t kMilliPerLine = 1000;
constexpr std::uint64_t kCapacity = LogRateLimiter::kBurst * kMilliPerLine;

[[nodiscard]] std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

}  // namespace

LogRateLimiter::Slot& LogRateLimiter::slot_for(std::uint64_t hash) noexcept {
    Slot* free_slot = nullptr;
    for (std::size_t i = 0; i < kMaxEvents; ++i) {
        Slot& slot = slots_[i];
        if (slot.used && slot.hash == hash) {
            return slot;
        }
        if (!slot.used && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    if (free_slot == nullptr) {
        return slots_[kMaxEvents];
    }
    free_slot->used = true;
    free_slot->hash = hash;
    return *free_slot;
}

LogRateLimiter::Decision LogRateLimiter::admit(std::string_view event, TimePoint now) noexcept {
    Slot& slot = slot_for(fnv1a(event));
    if (!slot.started) {
        slot.started = true;
        slot.millitokens = kCapacity;
        slot.last = now;
    } else if (now > slot.last) {
        // One line per second is one milli-line per millisecond. A clock that goes backwards
        // gives and takes nothing.
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.last).count());
        slot.millitokens = std::min(kCapacity, slot.millitokens + std::min(elapsed, kCapacity));
        slot.last = now;
    }
    if (slot.millitokens >= kMilliPerLine) {
        slot.millitokens -= kMilliPerLine;
        const Decision decision{true, slot.suppressed};
        slot.suppressed = 0;
        return decision;
    }
    if (slot.suppressed < std::numeric_limits<std::uint64_t>::max()) {
        ++slot.suppressed;
    }
    return {};
}

}  // namespace wsrv
