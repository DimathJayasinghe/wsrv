// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/syscalls.hpp"

#include <cstdint>

namespace wsrv {

/// What the process limit was, and whether we raised it.
struct FdBudget {
    std::uint64_t soft = 0;  ///< the soft limit in force after the check
    bool raised = false;     ///< true when the soft limit had to be raised to reach `needed`
};

/// Make sure the process may hold `needed` descriptors (ADR-0022, SECURITY_DESIGN T4, OPERATIONS
/// section 4). A soft limit that is already enough is left alone; a lower one is raised to exactly
/// `needed` when the hard limit allows it (never past what is needed, never the hard limit itself).
/// @return Error{FdLimit} when the hard limit is too low or the kernel refuses the raise; the
///         caller must then not start: the server would fail with EMFILE under load instead.
///         Error{Io} when the limit cannot be read.
[[nodiscard]] Result<FdBudget> ensure_fd_budget(ISyscalls& sys, std::uint64_t needed);

}  // namespace wsrv
