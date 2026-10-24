// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/random.hpp"

namespace wsrv {

/// IRandom backed by getrandom(2): the kernel CSPRNG, never the C library's weak generator, never a
/// silent fallback. Thread-affinity: any-thread (getrandom is thread-safe; the object has no
/// state).
class SystemRandom final : public IRandom {
public:
    [[nodiscard]] Result<void> fill(MutableByteView out) override;
};

}  // namespace wsrv
