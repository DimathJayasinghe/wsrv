// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace wsrv {

/// @brief Library version, "MAJOR.MINOR.PATCH", taken from project(VERSION ...) in CMake.
/// @return A view of a string with static storage duration (always valid).
/// Thread-affinity: any-thread.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace wsrv
