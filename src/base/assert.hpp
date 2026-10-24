// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace wsrv::detail {

/// Print "WSRV_ASSERT failed: <condition> (<file>:<line>)" to stderr and abort. Never returns.
[[noreturn]] void assert_failed(const char* condition, const char* file, int line) noexcept;

}  // namespace wsrv::detail

/// Check an invariant or precondition whose violation would mean memory unsafety or a logic bug.
/// Always active (also in release builds): continuing past a broken invariant is worse than
/// stopping (docs/CODING_STANDARDS.md section 4). Do NOT use for conditions a peer can trigger:
/// return a Result.
#define WSRV_ASSERT(condition)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ::wsrv::detail::assert_failed(#condition, __FILE__, __LINE__);                         \
        }                                                                                          \
    } while (false)
