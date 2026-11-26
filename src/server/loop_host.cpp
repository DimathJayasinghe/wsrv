// SPDX-License-Identifier: Apache-2.0
#include "server/loop_host.hpp"

#include <exception>
#include <string>

namespace wsrv {

bool LoopHost::log_enabled(LogLevel level) const noexcept {
    return logger_.enabled(level);
}

void LoopHost::log(const LogRecord& record) {
    try {
        logger_.log(record);
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): logging must never fail a
                                       // connection
    }
}

}  // namespace wsrv
