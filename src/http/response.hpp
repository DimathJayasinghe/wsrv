// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "http/request.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wsrv {

/// An HTTP/1.1 response head (no body: handshake replies carry none).
struct HttpResponse {
    int status = 200;
    std::vector<HttpHeader> headers;
};

/// Standard reason phrase for the statuses this server emits; empty for any other status.
/// Thread-affinity: any-thread.
[[nodiscard]] std::string_view reason_phrase(int status) noexcept;

/// Serialise `response` as "HTTP/1.1 <status> <reason>\r\n<name>: <value>\r\n...\r\n".
/// Fails closed with Error{InvalidArgument} if the status is outside 100..599, a header name is not
/// a token, or a header value contains CR, LF, NUL or another control character (response
/// splitting, SECURITY_DESIGN T9). Nothing implicit is added (no Content-Length, no Connection).
/// May allocate.
[[nodiscard]] Result<std::string> serialize(const HttpResponse& response);

}  // namespace wsrv
