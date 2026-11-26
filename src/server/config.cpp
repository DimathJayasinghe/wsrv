// SPDX-License-Identifier: Apache-2.0
#include "server/config.hpp"

#include "http/token.hpp"

#include <algorithm>
#include <optional>

namespace wsrv {

namespace {

[[nodiscard]] Error bad_config(std::string_view field) {
    return Error{ErrorCode::InvalidConfig, field};
}

/// A mandatory timer: strictly positive and not longer than the clock arithmetic tolerates.
[[nodiscard]] bool timer_ok(std::chrono::milliseconds value) {
    return value > std::chrono::milliseconds::zero() && value <= kMaxConfiguredDuration;
}

/// The two optional listeners (ADR-0023, ADR-0028): their limits only matter when they are on.
[[nodiscard]] Result<void> validate_listeners(const ServerConfig& config) {
    if (config.admin.has_value() &&
        (config.admin_max_connections == 0 ||
         config.admin_max_connections > kMaxConfiguredAdminConnections ||
         config.admin_request_timeout <= std::chrono::milliseconds{0} ||
         config.admin_request_timeout > kMaxAdminRequestTimeout)) {
        return std::unexpected{bad_config("admin limits out of range (ADR-0023)")};
    }
    if (config.publish.has_value() &&
        (config.publish_max_connections == 0 ||
         config.publish_max_connections > kMaxConfiguredPublishConnections ||
         config.publish_request_timeout <= std::chrono::milliseconds{0} ||
         config.publish_request_timeout > kMaxPublishRequestTimeout ||
         config.publish_max_body_bytes == 0 ||
         config.publish_max_body_bytes > kMaxConfiguredPublishBody)) {
        return std::unexpected{bad_config("publish limits out of range (ADR-0028)")};
    }
    return {};
}

/// The two byte budgets: direct sends waiting for a loop (ADR-0032) and everything the connections
/// buffer together (ADR-0040). Neither has an unlimited value.
[[nodiscard]] Result<void> validate_byte_budgets(const ServerConfig& config) {
    if (config.max_pending_send_bytes == 0 || config.max_pending_send_bytes > kMaxConfiguredQueue) {
        return std::unexpected{
            bad_config("max_pending_send_bytes must be in (0, 1 GiB] (ADR-0032)")};
    }
    if (config.max_buffered_bytes == 0 || config.max_buffered_bytes > kMaxConfiguredBufferedBytes) {
        return std::unexpected{bad_config("max_buffered_bytes must be in (0, 1 TiB] (ADR-0040)")};
    }
    return {};
}

[[nodiscard]] Result<void> validate_limits(const ServerConfig& config) {
    if (const Result<void> listeners = validate_listeners(config); !listeners) {
        return listeners;
    }
    if (config.max_connections == 0 || config.max_connections > kMaxConfiguredConnections) {
        return std::unexpected{bad_config("max_connections out of range")};
    }
    if (config.threads == 0 || config.threads > kMaxConfiguredThreads) {
        return std::unexpected{bad_config("threads out of range")};
    }
    if (config.max_connections_per_ip > kMaxConfiguredConnections) {
        return std::unexpected{bad_config("max_connections_per_ip out of range (ADR-0021)")};
    }
    if (config.max_new_connections_per_ip_per_second > kMaxConfiguredNewConnectionsPerSecond) {
        return std::unexpected{
            bad_config("max_new_connections_per_ip_per_second out of range (ADR-0021)")};
    }
    if (config.trusted_proxies.size() > kMaxTrustedProxies) {
        return std::unexpected{bad_config("trusted_proxies has too many entries (ADR-0029)")};
    }
    if (!is_token(config.client_ip_header)) {
        return std::unexpected{bad_config("client_ip_header is not a header name (ADR-0029)")};
    }
    if (config.http.max_request_line == 0 || config.http.max_total_bytes == 0 ||
        config.http.max_headers == 0) {
        return std::unexpected{bad_config("http limits must be non-zero")};
    }
    if (const Result<void> budgets = validate_byte_budgets(config); !budgets) {
        return budgets;
    }
    if (config.read.max_reads_per_event == 0 || config.read.max_bytes_per_event == 0 ||
        config.read.max_bytes_per_event > kMaxConfiguredReadBytes) {
        return std::unexpected{bad_config("read budget out of range")};
    }
    return {};
}

/// permessage-deflate needs both halves (ADR-0020): the handshake policy decides what is offered to
/// clients, the session settings hold the codecs that serve it. A policy without codecs would
/// advertise an extension the session cannot speak; codecs without a policy are dead weight that
/// look like a working feature. Either way the operator meant something else, so refuse.
[[nodiscard]] Result<void> validate_deflate_pairing(const ServerConfig& config) {
    const std::optional<DeflatePolicy>& policy = config.handshake.permessage_deflate;
    if (policy.has_value() != config.session.deflate.has_value()) {
        return std::unexpected{bad_config("permessage_deflate: the handshake policy and the "
                                          "session codecs must be set together (ADR-0020)")};
    }
    if (policy && !valid(*policy)) {
        return std::unexpected{bad_config("permessage_deflate: window sizes must be 9..15")};
    }
    return {};
}

/// The ADR-0018 hook settings. Checked even when no decider is injected, so a config that would be
/// refused with the hook on is never silently accepted with it off.
[[nodiscard]] Result<void> validate_hook(const ServerConfig& config) {
    if (config.handshake_decision_timeout < kMinDecisionTimeout ||
        config.handshake_decision_timeout > kMaxDecisionTimeout) {
        return std::unexpected{
            bad_config("handshake_decision_timeout must be in [100 ms, 60 s] (ADR-0018)")};
    }
    if (config.max_pending_handshakes == 0 ||
        config.max_pending_handshakes > kMaxConfiguredConnections) {
        return std::unexpected{
            bad_config("max_pending_handshakes must be in (0, 1048576] (ADR-0018)")};
    }
    if (config.handshake_forward_headers.size() > kMaxForwardedHeaderNames) {
        return std::unexpected{bad_config("handshake_forward_headers has too many names")};
    }
    for (const std::string& name : config.handshake_forward_headers) {
        if (!is_token(name)) {
            return std::unexpected{
                bad_config("handshake_forward_headers has a name that is not a header token")};
        }
    }
    return {};
}

}  // namespace

std::size_t effective_max_pending_handshakes(const ServerConfig& config) noexcept {
    return std::min(config.max_pending_handshakes, config.max_connections);
}

Result<void> validate(const ServerConfig& config) {
    if (const Result<void> limits = validate_limits(config); !limits) {
        return limits;
    }
    if (!timer_ok(config.handshake_timeout)) {
        return std::unexpected{bad_config("handshake_timeout must be in (0, 24 h] (T2)")};
    }
    if (!timer_ok(config.write_stall_timeout)) {
        return std::unexpected{bad_config("write_stall_timeout must be in (0, 24 h] (T3)")};
    }
    if (!timer_ok(config.shutdown_grace)) {
        return std::unexpected{bad_config("shutdown_grace must be in (0, 24 h] (FR-S5)")};
    }
    if (const Result<void> hook = validate_hook(config); !hook) {
        return hook;
    }
    if (const Result<void> deflate = validate_deflate_pairing(config); !deflate) {
        return deflate;
    }
    return validate(config.session);
}

}  // namespace wsrv
