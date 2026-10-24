// SPDX-License-Identifier: Apache-2.0
#include "base/assert.hpp"

#include <cstdio>
#include <cstdlib>

namespace wsrv::detail {

void assert_failed(const char* condition, const char* file, int line) noexcept {
    // The failure path is deliberately minimal and allocation-free (std::print may allocate), and
    // there is nothing useful to do if stderr itself fails.
    // NOLINTNEXTLINE(modernize-use-std-print,cert-err33-c)
    std::fprintf(stderr, "WSRV_ASSERT failed: %s (%s:%d)\n", condition, file, line);
    std::abort();
}

}  // namespace wsrv::detail
