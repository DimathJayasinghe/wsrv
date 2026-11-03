// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace wsrv::wsrvd {

bool constant_time_equal(ByteView a, ByteView b) noexcept {
    const std::size_t longest = std::max(a.size(), b.size());
    // A different length is a difference like any other: fold it in instead of returning early.
    auto difference = static_cast<std::uint32_t>(a.size() != b.size());
    for (std::size_t i = 0; i < longest; ++i) {
        const std::uint8_t left = i < a.size() ? std::to_integer<std::uint8_t>(a[i]) : 0U;
        const std::uint8_t right = i < b.size() ? std::to_integer<std::uint8_t>(b[i]) : 0U;
        difference |= static_cast<std::uint32_t>(left ^ right);
    }
    return difference == 0;
}

}  // namespace wsrv::wsrvd
