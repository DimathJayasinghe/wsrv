// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "http/request.hpp"

#include <optional>
#include <string_view>

namespace wsrv {

/// The credential of `Authorization: Bearer <credential>` in a parsed request head, if the head
/// carries exactly one acceptable one (ADR-0041 section 4; the publish endpoint, ADR-0028, and the
/// admin listener share this single definition so the two cannot drift).
///
/// Acceptable means: exactly ONE Authorization header (two are ambiguous even when equal: which
/// does a proxy forward, which does the server judge?), the scheme `Bearer` in any case (RFC 9110
/// section 11.1), exactly one space, then a non-empty credential with no space and no control
/// character in it. Anything else is "no credential"; the endpoints answer 401 to all of it, so a
/// peer learns nothing about which rule it broke.
///
/// This only judges the shape. Whether the credential is the right one is the caller's decision
/// (constant_time_equal, `base/`), made before it spends anything on the request.
///
/// The view points into `head` and is valid while `head` is alive and unchanged. May throw
/// std::bad_alloc (HttpRequest::values() builds a small vector); the endpoints already catch it
/// and drop the client.
/// Thread-affinity: pure, any-thread.
[[nodiscard]] std::optional<std::string_view> bearer_credential(const HttpRequest& head);

}  // namespace wsrv
