// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"

namespace wsrv {

/// Compare two secrets without revealing, through timing, where they first differ (ADR-0017
/// section 4; also the publish token of ADR-0028). A normal comparison stops at the first different
/// byte, so an attacker who can measure many attempts learns the token one byte at a time. This one
/// always looks at max(a.size(), b.size()) bytes and folds every difference into one value, and a
/// length mismatch does not return early either. Thread-affinity: pure, any-thread.
[[nodiscard]] bool constant_time_equal(ByteView a, ByteView b) noexcept;

}  // namespace wsrv
