// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"

#include <concepts>
#include <limits>

namespace wsrv {

/// Overflow-checked addition. Use for every sum involving wire-derived values (offsets, lengths).
/// @return a + b, or Error{Overflow} if the true result does not fit in T. Thread-affinity:
/// any-thread.
template <std::unsigned_integral T>
[[nodiscard]] constexpr Result<T> checked_add(T a, T b) noexcept {
    if (a > std::numeric_limits<T>::max() - b) {
        return fail(ErrorCode::Overflow, "addition overflow");
    }
    return static_cast<T>(a + b);
}

/// Overflow-checked multiplication. The guard runs BEFORE the multiplication, so no intermediate
/// operation can overflow (including narrow types that promote to a signed int).
template <std::unsigned_integral T>
[[nodiscard]] constexpr Result<T> checked_mul(T a, T b) noexcept {
    if (a != 0 && b > std::numeric_limits<T>::max() / a) {
        return fail(ErrorCode::Overflow, "multiplication overflow");
    }
    return static_cast<T>(a * b);
}

}  // namespace wsrv
