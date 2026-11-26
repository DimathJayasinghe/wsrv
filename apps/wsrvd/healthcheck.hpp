// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include "wsrvd/config.hpp"

namespace wsrv::wsrvd {

/// Where a running daemon can be probed (ADR-0030 section 2). A port of 0 means "not known": the
/// kernel chose it, so nobody can be asked.
struct HealthTarget {
    std::string ws_host;  ///< listen_host; an unspecified address (0.0.0.0, ::) is probed on loopback
    std::uint16_t ws_port = 0;
    std::string ipc_socket;
    std::string admin_host;  ///< used only when admin_port is not 0
    std::uint16_t admin_port = 0;
};

/// The places the configured daemon listens on.
[[nodiscard]] HealthTarget health_target(const DaemonConfig& config);

/// Probe a running daemon: with an admin port `GET /healthz` must answer 200; otherwise the
/// WebSocket port and the IPC socket must both accept a connection. Bounded: the whole check gives
/// up after `deadline`, reads at most the first 12 bytes of an answer and allocates nothing from it.
/// @return Error{Io} naming the failed part ("websocket", "ipc", "healthz") or Error{InvalidConfig}
/// when there is nothing to probe (no known port). Thread-affinity: any thread; blocks.
[[nodiscard]] Result<void> check_health(const HealthTarget& target,
                                        std::chrono::milliseconds deadline);

}  // namespace wsrv::wsrvd
