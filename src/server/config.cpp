// SPDX-License-Identifier: Apache-2.0
#include "server/config.hpp"

#include "http/token.hpp"

#include <algorithm>

namespace wsrv {

namespace {

[[nodiscard]] Error bad_config(std::string_view field) {
    return Error{ErrorCode::InvalidConfig, field};
}

/// A mandatory timer: strictly positive and not longer than the clock arithmetic tolerates.
[[nodiscard]] bool timer_ok(std::chrono::milliseconds value) {
    return value > std::chrono::milliseconds::zero() && value <= kMaxConfiguredDuration;
}

[[nodiscard]] Result<void> validate_limits(const ServerConfig& config) {
    if (config.max_connections == 0 || config.max_connections > kMaxConfiguredConnections) {
        return std::unexpected{bad_config("max_connections out of range")};
    }
    if (config.threads == 0 || config.threads > kMaxConfiguredThreads) {
        return std::unexpected{bad_config("threads out of range")};
    }
    if (config.http.max_request_line == 0 || config.http.max_total_bytes == 0 ||
        config.http.max_headers == 0) {
        return std::unexpected{bad_config("http limits must be non-zero")};
    }
    if (config.read.max_reads_per_event == 0 || config.read.max_bytes_per_event == 0 ||
        config.read.max_bytes_per_event > kMaxConfiguredReadBytes) {
        return std::unexpected{bad_config("read budget out of range")};
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
    return validate(config.session);
}

}  // namespace wsrv
