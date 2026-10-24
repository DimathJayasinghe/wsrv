// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

namespace wsrv {

/// Source of cryptographically secure random bytes (ADR-0012). The OS-backed implementation lives
/// in net/ (SystemRandom); logic code receives an IRandom, so it stays free of OS calls and
/// testable. Thread-affinity: implementations document their own.
class IRandom {
public:
    IRandom() = default;
    IRandom(const IRandom&) = delete;
    IRandom& operator=(const IRandom&) = delete;
    IRandom(IRandom&&) = delete;
    IRandom& operator=(IRandom&&) = delete;
    virtual ~IRandom() = default;

    /// Fill all of `out` with random bytes, or fail without guaranteeing anything about `out`.
    /// Implementations never fall back to a weak source. @return Error{RandomUnavailable} on
    /// failure.
    [[nodiscard]] virtual Result<void> fill(MutableByteView out) = 0;
};

}  // namespace wsrv
