// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "http/request_parser.hpp"
#include "net/endpoint.hpp"
#include "protocol/handshake.hpp"
#include "protocol/session.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace wsrv {

/// What to do with a client that does not read what we send (EC-N09, SECURITY_DESIGN T3).
enum class SlowConsumerPolicy : std::uint8_t {
    Close,        ///< close 1008 when the outbound queue is full (default)
    DropMessage,  ///< refuse the message (send_* returns Backpressure) and keep the connection
};

/// How much one readiness event may read before the connection yields to the others (EC-N07,
/// EC-N08). Edge-triggered epoll reports data once, so a connection that stops early must
/// reschedule itself; the budget is what keeps a flooding client from starving its neighbours.
struct ReadBudget {
    std::size_t max_reads_per_event = 16;
    std::size_t max_bytes_per_event = std::size_t{64} * 1024;
};

/// Everything configurable about one server, as plain data. Defaults are REQUIREMENTS section 4.
/// Build it with ServerBuilder (which calls validate() and throws ConfigError); nothing outside
/// the composition root constructs a Server from an unvalidated config.
struct ServerConfig {
    Endpoint listen;
    std::size_t max_connections = 10000;  ///< further connections are accepted and closed (FR-S1)

    HttpLimits http;  ///< the request head (FR-H7)
    HandshakeConfig
        handshake;          ///< Origin policy, subprotocols, rules (validated by the negotiator)
    SessionConfig session;  ///< frame and message limits, timers, rate limits

    std::chrono::milliseconds handshake_timeout{5000};  ///< from accept to a complete request head
    std::chrono::milliseconds write_stall_timeout{30000};  ///< queued output and no byte accepted
    std::chrono::milliseconds shutdown_grace{10000};       ///< graceful shutdown deadline (FR-S5)

    SlowConsumerPolicy slow_consumer = SlowConsumerPolicy::Close;
    ReadBudget read;
};

/// Hard ceilings (SECURITY_DESIGN section 3): a misconfiguration is rejected, never honoured.
inline constexpr std::size_t kMaxConfiguredConnections = std::size_t{1} << 20;
inline constexpr std::size_t kMaxConfiguredReadBytes = std::size_t{16} << 20;

/// Check a ServerConfig, including its SessionConfig and HttpLimits. Durations within
/// (0, 24 h] for the three server timers (each is a mandatory defence: handshake timeout T2,
/// write stall T3, shutdown grace FR-S5); max_connections in (0, kMaxConfiguredConnections];
/// HTTP limits non-zero; read budget in (0, ceiling].
/// @return Error{InvalidConfig} whose context names the offending field. The HandshakeConfig is
/// not checked here: HandshakeNegotiator validates it when it is constructed.
[[nodiscard]] Result<void> validate(const ServerConfig& config);

}  // namespace wsrv
