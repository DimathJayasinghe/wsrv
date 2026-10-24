// SPDX-License-Identifier: Apache-2.0
#include "net/system_random.hpp"

#include <cerrno>
#include <cstddef>

#include <sys/random.h>

namespace wsrv {

Result<void> SystemRandom::fill(MutableByteView out) {
    std::size_t done = 0;
    while (done < out.size()) {
        const MutableByteView rest = out.subspan(done);
        const ssize_t got = ::getrandom(rest.data(), rest.size(), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal before any byte was produced: retry
            }
            return fail(ErrorCode::RandomUnavailable, "getrandom failed");
        }
        if (got == 0) {
            return fail(ErrorCode::RandomUnavailable, "getrandom returned no data");
        }
        done += static_cast<std::size_t>(got);  // short reads are legal for large requests: loop
    }
    return {};
}

}  // namespace wsrv
