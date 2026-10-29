// SPDX-License-Identifier: Apache-2.0
#include "server/loop_host.hpp"

#include <exception>
#include <string>

namespace wsrv {

void LoopHost::log(LogLevel level, ConnectionId id, std::string_view message) {
    try {
        std::string line = "conn=" + std::to_string(id) + " ";
        line.append(message);
        logger_.log(level, line);
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): logging must never fail a
                                       // connection
    }
}

}  // namespace wsrv
