// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "http/request_parser.hpp"
#include "net/endpoint.hpp"
#include "protocol/handshake.hpp"
#include "protocol/outbound_scheduler.hpp"
#include "protocol/session.hpp"
#include "server/client_ip.hpp"
#include "server/pending_bytes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/// The budget for the bytes all connections buffer together (ADR-0040): 1 GiB by default, at most
/// 1 TiB. wsrv targets 64-bit hosts (epoll, Linux).
static_assert(sizeof(std::size_t) >= 8, "wsrv targets 64-bit hosts");
inline constexpr std::size_t kDefaultMaxBufferedBytes = std::size_t{1} << 30;
inline constexpr std::size_t kMaxConfiguredBufferedBytes = std::size_t{1} << 40;

/// Everything configurable about one server, as plain data. Defaults are REQUIREMENTS section 4.
/// Build it with ServerBuilder (which calls validate() and throws ConfigError); nothing outside
/// the composition root constructs a Server from an unvalidated config.
struct ServerConfig {
    Endpoint listen;
    std::size_t max_connections = 10000;  ///< further connections are accepted and closed (FR-S1)
    /// Per client address (IPv4, or the /64 of an IPv6 address; ADR-0021): live connections, and
    /// new connections per second (a connection performs one handshake). `0` = no limit. Over a
    /// limit the client gets a 429. Behind a reverse proxy every user is one address: raise or
    /// switch off.
    std::size_t max_connections_per_ip = 100;
    /// Put the client address in log lines (ADR-0037 section 4): off by default, an address is
    /// personal data.
    bool log_peer = false;
    std::uint32_t max_new_connections_per_ip_per_second = 50;
    /// Reverse proxies whose forwarded-address header is believed (ADR-0029). Empty = off: the TCP
    /// peer is always the client. A connection from one of these takes no per-IP slot at accept;
    /// its client address (from `client_ip_header`) is admitted after the upgrade request.
    std::vector<IpNet> trusted_proxies;
    /// The header that names the client: X-Forwarded-For is read as a list, any other as one value.
    std::string client_ip_header = "X-Forwarded-For";
    /// The admin listener for /metrics, /healthz and /readyz (ADR-0023). Absent = off. Plain HTTP
    /// and unauthenticated: keep it on loopback or behind a network policy.
    std::optional<Endpoint> admin;
    std::size_t admin_max_connections = 8;  ///< served at once; more are closed (1..64)
    std::chrono::milliseconds admin_request_timeout{5000};  ///< a request head must end by then
    /// The publish listener (ADR-0028): `POST /publish` with a bearer token. Absent = off. The
    /// token and the publish action come from the composition root (ServerDeps::publish), never
    /// from the config, so a config file or a command line cannot carry the secret.
    std::optional<Endpoint> publish;
    std::size_t publish_max_connections = 16;  ///< served at once; more are closed (1..64)
    std::chrono::milliseconds publish_request_timeout{5000};  ///< head and body must end by then
    std::size_t publish_max_body_bytes =
        std::size_t{64} * 1024;  ///< 1 .. kMaxConfiguredPublishBody
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
    /// Per loop: payload bytes of direct sends (SessionHandle::send_text / send_binary) that may
    /// wait for the loop at once (ADR-0032). 1 .. kMaxConfiguredQueue; no unlimited value.
    std::size_t max_pending_send_bytes = kDefaultMaxPendingSendBytes;
    /// The whole server: the most bytes its connections may hold in partial inbound messages and
    /// outbound queues together before new clients are refused with a 503 (ADR-0040). A soft cap:
    /// connections that exist are left alone. 1 .. kMaxConfiguredBufferedBytes; no unlimited value.
    std::size_t max_buffered_bytes = kDefaultMaxBufferedBytes;
};

/// Hard ceilings (SECURITY_DESIGN section 3): a misconfiguration is rejected, never honoured.
inline constexpr std::size_t kMaxConfiguredConnections = std::size_t{1} << 20;
inline constexpr std::size_t kMaxConfiguredThreads = 256;
inline constexpr std::uint32_t kMaxConfiguredNewConnectionsPerSecond = 1'000'000;
inline constexpr std::size_t kMaxConfiguredAdminConnections = 64;
inline constexpr std::chrono::milliseconds kMaxAdminRequestTimeout{60000};
inline constexpr std::size_t kMaxConfiguredPublishConnections = 64;
inline constexpr std::chrono::milliseconds kMaxPublishRequestTimeout{60000};
inline constexpr std::size_t kMaxConfiguredPublishBody = std::size_t{1} << 20U;  // 1 MiB
inline constexpr std::size_t kMaxConfiguredReadBytes = std::size_t{16} << 20;
inline constexpr std::chrono::milliseconds kMinDecisionTimeout{100};
inline constexpr std::chrono::milliseconds kMaxDecisionTimeout{60000};
inline constexpr std::size_t kMaxForwardedHeaderNames = 64;

/// Check a ServerConfig, including its SessionConfig and HttpLimits. Durations within
/// (0, 24 h] for the three server timers (each is a mandatory defence: handshake timeout T2,
/// write stall T3, shutdown grace FR-S5); max_connections in (0, kMaxConfiguredConnections];
/// per-IP limits within their ceilings (a limit of 0 means "off"); HTTP limits non-zero; read
/// budget in (0, ceiling]; permessage-deflate set in both places or in neither
/// (`handshake.permessage_deflate` and `session.deflate`, ADR-0020).
/// @return Error{InvalidConfig} whose context names the offending field. The rest of the
/// HandshakeConfig is not checked here: HandshakeNegotiator validates it when it is constructed.
[[nodiscard]] Result<void> validate(const ServerConfig& config);

/// The number of upgrades that may wait for the application at once: `max_pending_handshakes`,
/// but never more than `max_connections` (ADR-0018 section 4).
[[nodiscard]] std::size_t effective_max_pending_handshakes(const ServerConfig& config) noexcept;

/// The largest message the server can ever send to one connection (ADR-0043): what its outbound
/// queue holds on an empty queue. A send or a publish over it is refused where it is issued.
[[nodiscard]] constexpr std::size_t largest_outbound_message(const ServerConfig& config) noexcept {
    return largest_message(config.session.outbound);
}

}  // namespace wsrv
