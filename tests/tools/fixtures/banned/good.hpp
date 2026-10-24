// SPDX-License-Identifier: Apache-2.0
#pragma once
// Prose such as "the operating system" or "random access" must not trigger the scan, and neither must
// identifiers that merely contain a banned name: operand, random_device, system_clock, my_strcpy_like.
#include <chrono>
#include <random>

inline auto now() { return std::chrono::system_clock::now(); }
inline unsigned entropy() { std::random_device rd; return rd(); }
