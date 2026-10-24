// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace wsrv {

/// Is `value` a valid HTTP/1.1 Host header value, uri-host [ ":" port ] (RFC 9112 section 3.2)?
///  - a registered name made of letters, digits, '-', '.', '_', '~' and valid %XX escapes
///    (deliberately stricter than the RFC 3986 grammar: DNS names never need the sub-delimiters,
///    and accepting them lets values such as "a,b" through);
///  - or a bracketed IPv6 literal such as "[2001:db8::1]" (an IPv4-mapped tail is allowed);
///  - an optional ":" port of 1 to 5 digits with a value of at most 65535.
/// A server MUST answer 400 to a request whose Host is invalid (RFC 9112 section 3.2). Pure,
/// any-thread.
[[nodiscard]] bool is_valid_host_header(std::string_view value);

}  // namespace wsrv
