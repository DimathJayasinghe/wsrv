// SPDX-License-Identifier: Apache-2.0
#include "net/fd_budget.hpp"

namespace wsrv {

Result<FdBudget> ensure_fd_budget(ISyscalls& sys, std::uint64_t needed) {
    FdLimits limits;
    if (const SysResult read = sys.get_fd_limits(limits); read.err != 0) {
        return fail(ErrorCode::Io, "net: cannot read the descriptor limit (RLIMIT_NOFILE)");
    }
    if (limits.soft >= needed) {
        return FdBudget{limits.soft, false};
    }
    if (limits.hard < needed) {
        return fail(ErrorCode::FdLimit,
                    "net: the hard descriptor limit (RLIMIT_NOFILE) is below max_connections plus "
                    "the reserve: raise it (ulimit -Hn, LimitNOFILE=) or lower max_connections");
    }
    if (const SysResult raise = sys.set_fd_soft_limit(needed); raise.err != 0) {
        return fail(ErrorCode::FdLimit,
                    "net: the kernel refused to raise the soft descriptor limit (RLIMIT_NOFILE)");
    }
    return FdBudget{needed, true};
}

}  // namespace wsrv
