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
#include <string>
#include <vector>

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
    std::size_t threads = 1;  ///< loops: loop 0 on the caller's thread, the rest started by run()

    HttpLimits http;  ///< the request head (FR-H7)
    HandshakeConfig
        handshake;          ///< Origin policy, subprotocols, rules (validated by the negotiator)
    SessionConfig session;  ///< frame and message limits, timers, rate limits

    std::chrono::milliseconds handshake_timeout{5000};  ///< from accept to a complete request head
    std::chrono::milliseconds write_stall_timeout{30000};  ///< queued output and no byte accepted
    std::chrono::milliseconds shutdown_grace{10000};       ///< graceful shutdown deadline (FR-S5)

    // The asynchronous handshake decision (ADR-0018). Used only when a decider is injected.
    /// How long a valid upgrade waits for the application before it is refused with 503.
    std::chrono::milliseconds handshake_decision_timeout{3000};
    /// How many upgrades may wait for the application at once, in the whole server.
    std::size_t max_pending_handshakes = 256;
    /// Request headers copied to the application with the handshake (compared case-insensitively).
    /// Cookie and Authorization are secrets: they are forwarded only if listed here.
    std::vector<std::string> handshake_forward_headers;

    SlowConsumerPolicy slow_consumer = SlowConsumerPolicy::Close;
    ReadBudget read;
};

/// Hard ceilings (SECURITY_DESIGN section 3): a misconfiguration is rejected, never honoured.
inline constexpr std::size_t kMaxConfiguredConnections = std::size_t{1} << 20;
inline constexpr std::size_t kMaxConfiguredThreads = 256;
inline constexpr std::size_t kMaxConfiguredReadBytes = std::size_t{16} << 20;
inline constexpr std::chrono::milliseconds kMinDecisionTimeout{100};
inline constexpr std::chrono::milliseconds kMaxDecisionTimeout{60000};
inline constexpr std::size_t kMaxForwardedHeaderNames = 64;

/// Check a ServerConfig, including its SessionConfig and HttpLimits. Durations within
/// (0, 24 h] for the three server timers (each is a mandatory defence: handshake timeout T2,
/// write stall T3, shutdown grace FR-S5); max_connections in (0, kMaxConfiguredConnections];
/// HTTP limits non-zero; read budget in (0, ceiling].
/// @return Error{InvalidConfig} whose context names the offending field. The HandshakeConfig is
/// not checked here: HandshakeNegotiator validates it when it is constructed.
[[nodiscard]] Result<void> validate(const ServerConfig& config);

/// The number of upgrades that may wait for the application at once: `max_pending_handshakes`,
/// but never more than `max_connections` (ADR-0018 section 4).
[[nodiscard]] std::size_t effective_max_pending_handshakes(const ServerConfig& config) noexcept;

}  // namespace wsrv
