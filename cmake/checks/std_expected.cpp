// SPDX-License-Identifier: Apache-2.0
// Compiled (not run) at configure time by cmake/WsrvToolchainChecks.cmake.
#include <cstddef>
#include <expected>
#include <span>

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "std::expected (C++23) is not available"
#endif

int main() {
    std::byte buffer[2]{};
    const std::span<const std::byte> view{buffer};
    const std::expected<std::size_t, int> result = view.size();
    return result.has_value() ? 0 : 1;
}
