// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace wsrv {

/// The admin page logic, without sockets (ADR-0023 section 3). The request head is peer data; the
/// answer is always short and well formed.

/// Longest request head the admin port reads. A head that is not complete by then is answered 400.
inline constexpr std::size_t kMaxAdminHeadBytes = 2048;

/// What the pages are made of. Both are called on the admin listener's loop thread.
struct AdminSources {
    std::function<std::string()> metrics_text;  ///< the Prometheus page
    std::function<bool()> ready;                ///< accepting and not shutting down
};

/// The answer to a head that never became complete within kMaxAdminHeadBytes: 400, always.
[[nodiscard]] std::string admin_bad_request();

/// True once `buffered` holds the blank line that ends a request head.
[[nodiscard]] bool admin_head_complete(std::string_view buffered) noexcept;

/// The complete HTTP/1.1 answer (status line, headers, body; always `Connection: close`) to the
/// request whose head is `head`: 200 for GET /metrics, /healthz, /readyz (503 when not ready), 404
/// for any other target (exact match: no query, no case folding, no normalisation), 405 for any
/// other method, 400 for anything that is not a well-formed HTTP/1.0 or 1.1 request line.
/// Headers and body of the request are never looked at.
[[nodiscard]] std::string admin_response(std::string_view head, const AdminSources& sources);

}  // namespace wsrv
