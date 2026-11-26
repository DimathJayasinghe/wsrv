// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace wsrv {

/// The admin page logic, without sockets (ADR-0023 section 3). The request head is peer data; the
/// answer is always short and well formed.

/// A token shorter than this is refused at start (ADR-0041): a guessable credential is no
/// credential. `wsrvd` asks for more (a token `--generate-token` made); this is the floor for an
/// embedder.
inline constexpr std::size_t kMinAdminTokenBytes = 32;

/// Longest request head the admin port reads. A head that is not complete by then is answered 400.
inline constexpr std::size_t kMaxAdminHeadBytes = 2048;

/// What the pages are made of. Everything here is called on the admin listener's loop thread.
struct AdminSources {
    std::function<std::string()> metrics_text;  ///< the Prometheus page
    std::function<bool()> ready;                ///< accepting and not shutting down
    /// ADR-0041: when not empty, `GET /metrics` needs `Authorization: Bearer <token>` and
    /// `/healthz` and `/readyz` stay open (a kubelet probe and `wsrvd --healthcheck` cannot send a
    /// credential). Empty = no credential is ever asked for or looked at.
    std::string token;
    /// Called once for every `/metrics` request refused for its credential (the caller logs it,
    /// rate-limited). May be empty.
    std::function<void()> unauthorized;
};

/// The answer to a head that never became complete within kMaxAdminHeadBytes: 400, always.
[[nodiscard]] std::string admin_bad_request();

/// True once `buffered` holds the blank line that ends a request head.
[[nodiscard]] bool admin_head_complete(std::string_view buffered) noexcept;

/// The complete HTTP/1.1 answer (status line, headers, body; always `Connection: close`) to the
/// request whose head is `head`: 200 for GET /metrics, /healthz, /readyz (503 when not ready), 404
/// for any other target (exact match: no query, no case folding, no normalisation), 405 for any
/// other method, 400 for anything that is not a well-formed HTTP/1.0 or 1.1 request line.
/// Without a token the headers and body of the request are never looked at. With one (ADR-0041),
/// `GET /metrics` is first routed as above and then needs exactly one acceptable bearer credential
/// equal to the token: the head goes through the strict head parser (a head it refuses is 400),
/// and a missing, wrong, repeated or malformed credential is one `401` answer, given before
/// `metrics_text` is called. May throw std::bad_alloc (the caller drops the client).
[[nodiscard]] std::string admin_response(std::string_view head, const AdminSources& sources);

}  // namespace wsrv
