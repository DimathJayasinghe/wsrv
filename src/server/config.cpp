// SPDX-License-Identifier: Apache-2.0
#include "server/config.hpp"

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

}  // namespace

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
    return validate(config.session);
}

}  // namespace wsrv
